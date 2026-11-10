#include "app/client_application.h"
#include "core/build_info.h"

#include <algorithm>

namespace app {
  namespace {
    constexpr auto updateInterval = std::chrono::hours{24};
    constexpr auto initialUpdateDelay = std::chrono::seconds{10};
    constexpr float updateButtonWidth = Style::controlHeightLg * 3;
    bool busy(updates::Phase phase) {
      return phase == updates::Phase::Checking
          || phase == updates::Phase::Downloading
          || phase == updates::Phase::Installing;
    }
  } // namespace
  bool ClientApplication::updateRestartBusy() const {
    return (!m_voiceState.channel.empty()
            && m_voiceState.phase != voice::Phase::Disconnected
            && m_voiceState.phase != voice::Phase::Failed)
        || !m_voiceMessageChannel.empty()
        || m_sharingPublicationRequested
        || m_sending
        || std::ranges::any_of(m_streamSnapshot.active, [](const auto& stream) { return stream.publishing; })
        || m_userSettingsLists.busy;
  }
  void ClientApplication::updateTick() {
    if (!m_demo) {
      if (m_preferences.updateChecks && !m_account.id.empty() && !m_updateTimer.active()) {
        m_updateTimer.start(initialUpdateDelay, [this] {
          if (!m_preferences.updateChecks || m_account.id.empty())
            return;
          m_updateAutomatic = m_updater.check(m_preferences.updatePrereleases);
          m_updateTimer.startRepeating(updateInterval, [this] {
            if (m_preferences.updateChecks && !m_account.id.empty())
              m_updateAutomatic = m_updater.check(m_preferences.updatePrereleases);
          });
        });
      } else if (!m_preferences.updateChecks || m_account.id.empty())
        m_updateTimer.stop();
      if (auto snapshot = m_updater.poll()) {
        m_updateSnapshot = std::move(*snapshot);
        if (m_updateSnapshot.phase == updates::Phase::Available
            && m_updateAutomatic
            && m_preferences.updateChecks
            && m_preferences.updateDownloads
            && m_updater.installation().kind == updates::InstallationKind::AppImage)
          if (m_updater.download())
            m_updateSnapshot.phase = updates::Phase::Downloading;
        if (m_updateSnapshot.phase == updates::Phase::Available && m_status.empty())
          m_status = "A Cressida update is available. Open Settings > Updates.";
        if (m_updateSnapshot.phase == updates::Phase::Ready && m_status.empty())
          m_status = "A Cressida update is ready. Restart from Settings > Updates when you are ready.";
        if (!busy(m_updateSnapshot.phase))
          m_updateAutomatic = false;
        m_window.invalidate();
      }
    }
    if (m_updateRestartPending && m_updateSnapshot.phase == updates::Phase::Installed) {
      m_updateRestartPending = false;
      if (updateRestartBusy()) {
        m_updateSnapshot.error =
            "The update is installed. Finish your call, recording, sharing or pending send before restarting.";
        m_window.invalidate();
      } else
        trayEvent({.command = TrayCommand::Restart});
    } else if (m_updateSnapshot.phase == updates::Phase::Error)
      m_updateRestartPending = false;
  }
  bool ClientApplication::updateAction(const std::string& action) {
    if (!action.starts_with("updates:"))
      return false;
    if (action == "updates:automatic") {
      m_preferences.updateChecks = !m_preferences.updateChecks;
      m_updateTimer.stop();
      if (!m_preferences.updateChecks && m_updateAutomatic) {
        m_updater.cancel();
        m_updateSnapshot.phase = updates::Phase::Idle;
        m_updateAutomatic = false;
      }
      savePreferences();
    } else if (action == "updates:download-automatically") {
      m_preferences.updateDownloads = !m_preferences.updateDownloads;
      if (!m_preferences.updateDownloads
          && m_updateAutomatic
          && m_updateSnapshot.phase == updates::Phase::Downloading) {
        m_updater.cancel();
        m_updateSnapshot.phase = updates::Phase::Idle;
        m_updateAutomatic = false;
      }
      savePreferences();
    } else if (action == "updates:prereleases") {
      if (busy(m_updateSnapshot.phase) || m_updateSnapshot.phase == updates::Phase::Installed)
        return true;
      m_preferences.updatePrereleases = !m_preferences.updatePrereleases;
      m_updateAutomatic = m_updateRestartPending = false;
      if (!m_demo)
        m_updater.cancel();
      m_updateSnapshot = {};
      savePreferences();
    } else if (action == "updates:check") {
      m_updateAutomatic = false;
      if (m_demo) {
        m_updateSnapshot.phase = updates::Phase::Checking;
        m_updateSnapshot.error.clear();
      } else
        m_updater.check(m_preferences.updatePrereleases);
    } else if (action == "updates:download") {
      if (m_demo) {
        m_updateSnapshot.phase = updates::Phase::Downloading;
        m_updateSnapshot.total = 100;
      } else if (m_updater.download())
        m_updateSnapshot.phase = updates::Phase::Downloading;
    } else if (action == "updates:cancel") {
      if (m_updateSnapshot.phase == updates::Phase::Installing || m_updateSnapshot.phase == updates::Phase::Installed)
        return true;
      if (!m_demo)
        m_updater.cancel();
      m_updateAutomatic = m_updateRestartPending = false;
      m_updateSnapshot.phase = updates::Phase::Idle;
      m_updateSnapshot.error.clear();
    } else if (action == "updates:restart") {
      if (updateRestartBusy()) {
        m_updateSnapshot.error = "Finish your call, recording, sharing or pending send before restarting.";
      } else if (m_updateSnapshot.phase == updates::Phase::Installed) {
        trayEvent({.command = TrayCommand::Restart});
      } else if (m_updateSnapshot.phase == updates::Phase::Ready) {
        m_updateRestartPending = true;
        if (m_demo)
          m_updateSnapshot.phase = updates::Phase::Installing;
        else if (m_updater.install())
          m_updateSnapshot.phase = updates::Phase::Installing;
        else
          m_updateRestartPending = false;
      }
    } else if (action == "updates:releases")
      activate(
          {.action = Action::OpenUrl,
           .id = m_updateSnapshot.release ? m_updateSnapshot.release->url : std::string(updates::releasesUrl)}
      );
    m_window.invalidate();
    return true;
  }
  bool ClientApplication::drawUpdateSettings(UserSettingsForm& form) {
    if (m_settingsPage != "updates")
      return false;
    const auto kind = m_updater.installation().kind;
    const auto phase = m_updateSnapshot.phase;
    const bool active = busy(phase);
    form.section("Cressida updates");
    form.row("Installed version", std::string(cressida::build_info::version()));
    form.row(updates::installationName(kind), updates::installationHelp(kind));
    std::string status;
    switch (phase) {
    case updates::Phase::Idle:
      status = "Check for a newer release when you are ready.";
      break;
    case updates::Phase::Checking:
      status = "Checking for updates…";
      break;
    case updates::Phase::Current:
      status = "No newer update was found for this installation and release channel.";
      break;
    case updates::Phase::Available:
      status = "An update is available.";
      break;
    case updates::Phase::Downloading:
      status = "Downloading update…";
      if (m_updateSnapshot.total)
        status += " " + std::to_string(m_updateSnapshot.downloaded * 100 / m_updateSnapshot.total) + "%";
      break;
    case updates::Phase::Ready:
      status = "Download verified. Restart when you are ready to install it.";
      break;
    case updates::Phase::Installing:
      status = "Preparing the update for restart…";
      break;
    case updates::Phase::Installed:
      status = "The update is installed. Restart to use the new version.";
      break;
    case updates::Phase::Error:
      status = "The update could not be completed. You can try again explicitly.";
      break;
    }
    form.row("Update status", status);
    if (!m_updateSnapshot.error.empty())
      form.row("Update information", m_updateSnapshot.error);
    form.button(
        "Check for updates", "updates:check", ButtonVariant::Default, updateButtonWidth,
        !active && phase != updates::Phase::Installed && phase != updates::Phase::Ready
    );
    const bool canDownload = kind == updates::InstallationKind::Flatpak
        || (kind == updates::InstallationKind::AppImage
            && m_updateSnapshot.release
            && !m_updateSnapshot.release->image.name.empty()
            && !m_updateSnapshot.release->checksumsUrl.empty());
    if ((phase == updates::Phase::Available || phase == updates::Phase::Error) && canDownload)
      form.button(
          kind == updates::InstallationKind::Flatpak ? "Install update" : "Download update", "updates:download",
          ButtonVariant::Primary, updateButtonWidth
      );
    if (phase == updates::Phase::Ready || phase == updates::Phase::Installed)
      form.button(
          "Restart to update", "updates:restart", ButtonVariant::Primary, updateButtonWidth, !updateRestartBusy()
      );
    if ((active && phase != updates::Phase::Installing) || phase == updates::Phase::Ready)
      form.button("Cancel update", "updates:cancel", ButtonVariant::Default, updateButtonWidth);
    form.button("View releases", "updates:releases", ButtonVariant::Default, updateButtonWidth);
    form.section("Update preferences");
    form.toggle(
        "updates:automatic", "Check automatically",
        "Check after sign-in, then once a day. Checks do not use your account credentials.", m_preferences.updateChecks,
        [this](bool) { updateAction("updates:automatic"); }
    );
    if (kind != updates::InstallationKind::Flatpak)
      form.toggle(
          "updates:prereleases", "Include prereleases",
          "Offer test releases alongside stable releases. Updates always require a newer version.",
          m_preferences.updatePrereleases, [this](bool) { updateAction("updates:prereleases"); },
          !active && phase != updates::Phase::Installed
      );
    if (kind == updates::InstallationKind::AppImage)
      form.toggle(
          "updates:download-automatically", "Download automatically",
          "When automatic checks find an update, download it in the background. Restart stays explicit.",
          m_preferences.updateDownloads, [this](bool) { updateAction("updates:download-automatically"); }
      );
    if (m_updateSnapshot.release) {
      form.section("Release " + m_updateSnapshot.release->version);
      form.row(
          "Release notes",
          m_updateSnapshot.release->notes.empty() ? "Open Releases to read the changes."
                                                  : m_updateSnapshot.release->notes
      );
    }
    return true;
  }
} // namespace app
