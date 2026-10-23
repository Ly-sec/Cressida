#include "auth/credentials.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace auth {
  namespace {
    constexpr Logger logger{"credentials"};
    constexpr const char* operationName(CredentialOperation operation) {
      switch (operation) {
      case CredentialOperation::Load:
        return "load";
      case CredentialOperation::Save:
        return "save";
      case CredentialOperation::Forget:
        return "forget";
      }
      return "unknown";
    }
    constexpr const char* failureName(CredentialFailure failure) {
      switch (failure) {
      case CredentialFailure::Unavailable:
        return "keyring-unavailable";
      case CredentialFailure::Cancelled:
        return "keyring-cancelled";
      case CredentialFailure::Invalid:
        return "invalid-credential";
      case CredentialFailure::StateFile:
        return "local-state";
      }
      return "unknown";
    }
  } // namespace
  bool validCredential(const std::string& token) {
    return !token.empty()
        && token.size() <= 4096
        && std::ranges::all_of(token, [](unsigned char c) { return c > 32 && c < 127; });
  }
  std::string credentialError(CredentialFailure failure) {
    switch (failure) {
    case CredentialFailure::Unavailable:
      return "Could not access your desktop keyring. Unlock it or enable a Secret Service provider to remember "
             "sign-in.";
    case CredentialFailure::Cancelled:
      return "Keyring access was cancelled or timed out. Sign in again to retry.";
    case CredentialFailure::Invalid:
      return "The saved login is invalid. Sign in again.";
    case CredentialFailure::StateFile:
      return "Could not update local login state. Check permissions on the Cressida state directory.";
    }
    return "Could not save your login.";
  }
  CredentialStore::CredentialStore(std::unique_ptr<CredentialBackend> backend, std::filesystem::path disabled)
      : m_backend(backend ? std::move(backend) : secretServiceBackend()), m_disabled(std::move(disabled)) {
    if (m_disabled.empty()) {
      if (const auto* state = std::getenv("XDG_STATE_HOME"); state && *state)
        m_disabled = std::filesystem::path(state) / "cressida/login-disabled";
      else if (const auto* home = std::getenv("HOME"); home && *home)
        m_disabled = std::filesystem::path(home) / ".local/state/cressida/login-disabled";
    }
    // Construction does not access the keyring or filesystem (including in demo).
    m_worker = std::jthread([this](std::stop_token stop) { run(stop); });
  }
  CredentialStore::~CredentialStore() { shutdown(); }
  void CredentialStore::shutdown() {
    {
      std::lock_guard lock(m_mutex);
      m_stopping = true;
      m_activeStop.request_stop();
      m_tasks.clear();
    }
    m_worker.request_stop();
    m_ready.notify_all();
    if (m_worker.joinable())
      m_worker.join();
  }
  bool CredentialStore::disableRestore() {
    if (m_disabled.empty())
      return false;
    std::error_code error;
    std::filesystem::create_directories(m_disabled.parent_path(), error);
    if (error)
      return false;
    auto temporary = (m_disabled.parent_path() / ".login-XXXXXX").string();
    const int fd = mkstemp(temporary.data());
    if (fd < 0)
      return false;
    constexpr char marker[] = "disabled\n";
    const bool written = fchmod(fd, 0600) == 0 && write(fd, marker, sizeof(marker) - 1) == sizeof(marker) - 1;
    const bool synced = written && fsync(fd) == 0;
    close(fd);
    if (synced) {
      std::filesystem::rename(temporary, m_disabled, error);
      if (!error)
        return true;
    }
    unlink(temporary.c_str());
    return false;
  }
  void CredentialStore::load(std::uint64_t request, Completion complete) {
    std::lock_guard lock(m_mutex);
    if (m_stopping)
      return;
    std::erase_if(m_tasks, [](const auto& task) { return task.operation == CredentialOperation::Load; });
    if (m_busy && m_active == CredentialOperation::Load)
      m_activeStop.request_stop();
    m_tasks.push_back({CredentialOperation::Load, request, m_revision, {}, std::move(complete)});
    m_ready.notify_all();
  }
  void CredentialStore::save(std::uint64_t request, std::string token, Completion complete) {
    std::lock_guard lock(m_mutex);
    if (m_stopping)
      return;
    std::erase_if(m_tasks, [](const auto& task) { return task.operation == CredentialOperation::Save; });
    if (m_busy && m_active != CredentialOperation::Forget)
      m_activeStop.request_stop();
    m_tasks.push_back({CredentialOperation::Save, request, ++m_revision, std::move(token), std::move(complete)});
    m_ready.notify_all();
  }
  void CredentialStore::forget(std::uint64_t request, Completion complete) {
    std::lock_guard lock(m_mutex);
    if (m_stopping)
      return;
    // Persist the barrier before returning, even if quit interrupts keyring deletion.
    const bool marker = disableRestore();
    m_activeStop.request_stop();
    m_tasks.clear();
    m_tasks.push_back({CredentialOperation::Forget, request, ++m_revision, {}, std::move(complete), marker});
    m_ready.notify_all();
  }
  void CredentialStore::cancelLoad() {
    std::lock_guard lock(m_mutex);
    std::erase_if(m_tasks, [](const auto& task) { return task.operation == CredentialOperation::Load; });
    if (m_busy && m_active == CredentialOperation::Load)
      m_activeStop.request_stop();
  }
  void CredentialStore::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
      Task task;
      std::stop_token operationStop;
      {
        std::unique_lock lock(m_mutex);
        m_ready.wait(lock, stop, [&] { return !m_tasks.empty(); });
        if (stop.stop_requested())
          return;
        task = std::move(m_tasks.front());
        m_tasks.pop_front();
        m_active = task.operation;
        m_busy = true;
        m_activeStop = std::stop_source{};
        operationStop = m_activeStop.get_token();
      }
      CredentialResult result{task.operation, task.request};
      std::optional<CredentialFailure> failure;
      const auto fail = [&](CredentialFailure reason) {
        failure = reason;
        result.error = credentialError(reason);
      };
      logger.debug("operation started operation={}", operationName(task.operation));
      try {
        if (task.operation == CredentialOperation::Load) {
          std::error_code error;
          const bool disabled = m_disabled.empty() || std::filesystem::exists(m_disabled, error);
          if (error)
            fail(CredentialFailure::StateFile);
          else if (!disabled) {
            auto token = m_backend->load(operationStop);
            if (!token)
              fail(token.error());
            else if (!token->empty() && !validCredential(*token)) {
              fail(CredentialFailure::Invalid);
              {
                std::lock_guard lock(m_mutex);
                if (task.revision == m_revision)
                  (void)disableRestore();
              }
              (void)m_backend->erase(operationStop);
            } else {
              result.token = std::move(*token);
              result.found = !result.token.empty();
            }
          }
        } else if (task.operation == CredentialOperation::Save) {
          bool allowed = false;
          {
            std::lock_guard lock(m_mutex);
            allowed = task.revision == m_revision && disableRestore();
          }
          if (!validCredential(task.token))
            fail(CredentialFailure::Invalid);
          else if (!allowed)
            fail(CredentialFailure::StateFile);
          else if (auto saved = m_backend->save(task.token, operationStop); !saved)
            fail(saved.error());
          else {
            std::lock_guard lock(m_mutex);
            if (task.revision == m_revision && !operationStop.stop_requested()) {
              std::error_code error;
              std::filesystem::remove(m_disabled, error);
              if (error)
                fail(CredentialFailure::StateFile);
            }
          }
        } else {
          auto erased = m_backend->erase(operationStop);
          if (!erased)
            fail(erased.error());
          else if (!task.markerWritten)
            fail(CredentialFailure::StateFile);
        }
      } catch (...) {
        fail(CredentialFailure::Unavailable);
      }
      bool deliver;
      {
        std::lock_guard lock(m_mutex);
        m_busy = false;
        deliver = !m_stopping && !operationStop.stop_requested() && task.revision == m_revision;
      }
      if (!deliver)
        logger.debug("operation discarded operation={}", operationName(task.operation));
      else if (failure) {
        if (*failure == CredentialFailure::Cancelled)
          logger.debug("operation cancelled operation={}", operationName(task.operation));
        else
          logger.warn("operation failed operation={} stage={}", operationName(task.operation), failureName(*failure));
      } else
        logger.debug("operation completed operation={}", operationName(task.operation));
      if (deliver && task.complete)
        task.complete(std::move(result));
    }
  }
} // namespace auth
