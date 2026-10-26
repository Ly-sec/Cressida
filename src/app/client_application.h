#pragma once

#include "api/assets.h"
#include "api/client.h"
#include "api/member_list_identity.h"
#include "api/message_search.h"
#include "app/autocomplete.h"
#include "app/channel_invite.h"
#include "app/channel_menu.h"
#include "app/chat_cache.h"
#include "app/desktop_notifications.h"
#include "app/desktop_tray.h"
#include "app/discord_link.h"
#include "app/emoji_catalog.h"
#include "app/file_picker.h"
#include "app/folder_motion.h"
#include "app/image_cache.h"
#include "app/layout_metrics.h"
#include "app/message_history.h"
#include "app/message_layout.h"
#include "app/message_log.h"
#include "app/read_acknowledgements.h"
#include "app/role_motion.h"
#include "app/scroll_motion.h"
#include "app/server_audit.h"
#include "app/server_commands.h"
#include "app/server_menu.h"
#include "app/server_settings.h"
#include "app/server_settings_form.h"
#include "app/session_store.h"
#include "app/text_edit.h"
#include "app/theme_runtime.h"
#include "app/timeline_anchor.h"
#include "app/typing_indicator.h"
#include "app/unread_timeline.h"
#include "app/user_account_view.h"
#include "app/user_settings_form.h"
#include "app/user_settings_lists.h"
#include "app/window.h"
#include "app/window_icons.h"
#include "auth/browser_login.h"
#include "core/text_clipboard.h"
#include "core/timer_manager.h"
#include "media/video_player.h"
#include "render/backend/gles_framebuffer.h"
#include "render/core/color.h"
#include "render/core/texture_handle.h"
#include "render/scene/image_node.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/input.h"
#include "ui/controls/select_popup_context.h"

#include <EGL/egl.h>
#include <chrono>
#include <map>
#include <set>

namespace app {
  class SingleInstance;

  class ClientApplication : private SelectPopupContext {
  public:
    ClientApplication(bool demo, std::string token, SingleInstance* instance = nullptr);
    ~ClientApplication();
    int run(bool smokeTest = false, bool startMinimized = false);
    bool restartRequested() const { return m_restartRequested; }

  private:
    static constexpr int chatLinkReplayFrames = 6;
    static constexpr int attachmentReplayFrames = 9;
    static constexpr int typingReplayFrames = 16;
    static constexpr int floatingReplayFrames = 8;
    static constexpr int dmHomeReplayFrames = 24;
    static constexpr int dmProfileReplayFrames = 13;
    static constexpr int cosmeticsReplayFrames = 13;
    static constexpr int composerReplyReplayFrames = 10;
    static constexpr int embedReplayFrames = 12;
    static constexpr int markdownReplayFrames = 5;
    static constexpr int replyGroupingReplayFrames = 3;
    static constexpr int mediaGalleryReplayFrames = 13;
    static constexpr int imageMenuReplayFrames = 13;
    static constexpr int trayReplayFrames = 4;
    static constexpr int voiceReplayFrames = 15;
    enum class Action {
      Settings,
      VoiceAction,
      DmPage,
      DmAction,
      GuildMenu,
      CopyServerId,
      ServerInvitePeople,
      ServerSettings,
      CloseServerSettings,
      ServerSettingsAction,
      ServerMenuDialogAction,
      CloseSettings,
      SettingsPage,
      AccountPreference,
      UserAccountAction,
      SaveAccountSettings,
      ResetAccountSettings,
      SaveProfile,
      ResetProfile,
      Preference,
      JumpUnread,
      JumpChannelMention,
      MarkRead,
      RetryHistory,
      JumpLatest,
      SetPresence,
      TogglePresence,
      SaveCustomStatus,
      None,
      AutoModActions,
      AutoModJump,
      AutoModReport,
      ImagePan,
      PlayVideo,
      PauseVideo,
      ExpandVideo,
      CloseVideo,
      SeekVideo,
      VolumeVideo,
      MuteVideo,
      ImageZoom,
      Close,
      Minimize,
      Maximize,
      ViewImage,
      CloseImage,
      ImageNext,
      ImagePrevious,
      Connect,
      DiscordLogin,
      ToggleToken,
      CancelLogin,
      RetryLogin,
      Demo,
      Token,
      Guild,
      Home,
      DirectMessage,
      Category,
      Channel,
      Folder,
      ToggleMembers,
      DmProfileRetry,
      Send,
      RestoreOutgoing,
      PasteClipboard,
      ToggleEmoji,
      ToggleStickers,
      StickerSection,
      SendSticker,
      RemoveSticker,
      StickerInfo,
      StickerRetry,
      InsertEmoji,
      PickEmoji,
      FavoriteEmoji,
      EmojiSection,
      EmojiRetry,
      CloseEmoji,
      ToggleGif,
      CloseGif,
      GifTab,
      GifRetry,
      SendGif,
      FavoriteGif,
      Composer,
      ViewAttachment,
      RemoveAttachment,
      CopyMessage,
      CopyCode,
      OpenUrl,
      RevealSpoiler,
      SelectMessage,
      SignOut,
      MessageMenu,
      UserMenu,
      ConversationMenu,
      ChannelMenu,
      ChannelListMenu,
      SidebarSubmit,
      SidebarCopyInvite,
      ChannelCreateType,
      ChannelCreatePrivate,
      ChannelCreateBack,
      ChannelCreateAccess,
      ChannelMoveSync,
      InviteEditLink,
      InviteBack,
      InviteGenerate,
      InviteCopy,
      InviteRecipient,
      InviteOption,
      InviteTemporary,
      InviteDropdown,
      ChannelEditPage,
      ResetChannelEdit,
      ChannelEditOption,
      ChannelPermissionTarget,
      UserProfile,
      Reply,
      CancelReply,
      ToggleReplyMention,
      AddReaction,
      ToggleReaction,
      EditMessage,
      SaveMessageEdit,
      CancelMessageEdit,
      CompleteText,
      DeleteMessage,
      CopyLink,
      CopyId,
      MentionUser,
      OpenDm,
      BlockUser,
      UnblockUser,
      AddFriend,
      RemoveFriend,
      SetNickname,
      MuteConversation,
      UnmuteConversation,
      PinConversation,
      UnpinConversation,
      SubmitDialog,
      CloseDialog,
      JumpReply,
      RevealBlocked,
      QuickReaction,
      ProfileExpand,
      ProfileMessage,
      ProfileEdit,
      ProfileFriend,
      ProfileUnblock,
      ProfileRetry,
      ProfileSpoiler,
      GuildProfile,
      GuildProfileRetry,
      JoinGuild,
      OpenPreviewGuild,
      BrowseThreads,
      OpenThread,
      ThreadTab,
      ThreadMore,
      ThreadRetry,
      CreateThread,
      JoinThread,
      LeaveThread,
      ReopenThread,
      ThreadParent,
      ForumSearch,
      ForumSort,
      ForumTag,
      ForumRetry,
      ForumMore,
      NewForumPost,
      ForumPostTag,
      ChannelInfo,
      EditChannel,
      AddChannelAccess,
      ChannelAccessMode,
      ChannelAccessTarget,
      SaveChannelEdit,
      SaveChannelAccess,
      ChannelDetailsRetry,
      ToggleChannelMute,
      ChannelPins,
      ChannelSearch,
      SearchSuggestion,
      ClearSearchHistory,
      Inbox,
      InboxAction,
      BookmarkMessage,
      RemindMessage,
      CloseChannelPanel,
      ChannelPanelNext,
      ChannelPanelPrevious,
      ChannelPanelRetry,
      ChannelPanelJump,
      MessageButton,
      DismissEphemeral,
      SubmitApplicationModal,
      RemoveFromLog,
      ClearEditHistory,
      CopyImage,
      SaveImage,
      CopyMediaLink,
      OpenMediaLink
    };
    struct Hit {
      float x, y, width, height;
      Action action;
      std::string id;
      bool contains(float px, float py) const { return px >= x && py >= y && px < x + width && py < y + height; }
    };
    void draw();
    void updateCompletion();
    void closeCompletion(bool dismiss = false);
    bool completionKey(const Key& key);
    bool completionClick(float x, float y);
    void chooseCompletion(std::size_t index);
    std::vector<TextReplacement> composerReplacements(std::string_view value);
    void drawCompletion(Node& root, float width, float height);
    void completionEvent(const api::Event& event);
    void demoCompletionStep();
    bool beginMessageEdit(const std::string& message);
    void cancelMessageEdit(bool restoreComposer = true);
    void submitMessageEdit();
    bool messageEditKey(const Key& key);
    float messageEditHeight(float width);
    void drawMessageEdit(Node& parent, float x, float y, float width);
    void demoMessageEditStep();
    void demoComposerReplyStep();
    void windowFocusChanged(bool focused);
    void focusChatComposer();
    void closeVideo();
    void openVideo(const std::string& source);
    void drawVideo(
        Node& parent, float x, float y, float width, float height, const std::string& source, const std::string& poster,
        bool expanded = false
    );
    void drawVideoViewer(Node& root, float width, float height);
    bool videoAction(const Hit& hit);
    bool videoKey(const Key& key);
    void videoMotion(float x, float y);
    void videoRelease();
    void toggleVideo();
    void setVideoExpanded(bool enabled);
    void demoVideoStep();
    void tick();
    void closeToTray();
    void beginTrayStartup();
    void trayEvent(TrayEvent event);
    void demoTrayStep();
    void demoTrayStartup(bool verify);
    void onKey(const Key& key);
    void insert(std::string text);
    void activate(const Hit& hit);
    void connect();
    void beginLogin();
    void restoreLogin();
    void demo();
    void prepareDemo();
    void resetGuildBootstrap();
    void arrangeGuildFolders();
    void saveExpandedFolders();
    void drawServerRail(Node& root, float bottom);
    static std::string initial(const std::string& name);
    FolderMotion::Clock::time_point folderMotionTime() const;
    void demoFolderAnimationStep();
    void demoRailDragStep();
    bool beginRailDrag(const Hit& hit);
    bool railDragMotion();
    bool tickRailDrag();
    void finishRailDrag(bool cancel = false);
    void drawRailDrag(Node& root);
    struct RailDrag {
      Hit source;
      float x, y;
      std::uint64_t revision;
      bool active = false;
      std::optional<api::GuildRailMove> drop;
      Hit target{};
      FolderMotion::Clock::time_point tick = FolderMotion::Clock::now(), hover = tick;
      std::string hoverFolder;
    };
    std::optional<RailDrag> m_railDrag;
    std::vector<Hit> m_railTargets;
    std::optional<api::GuildRailMove> m_demoRailMove;
    bool m_railSaving = false;
    float m_railBottom = 0;
    void guildListEvent(api::Event& event);
    void guildLayoutEvent(api::Event& event);
    void channelSnapshotEvent(api::Event& event);
    void demoConnectionStep();
    void selectGuild(const std::string& id);
    void showDirectMessages(bool home = false);
    std::vector<api::Channel> dmConversations() const;
    bool dmMatches(const std::string& text) const;
    int messageRequestCount() const;
    void openDmPage(const std::string& page);
    void drawDirectMessagesButton(Node& root);
    void dmAction(const std::string& action);
    void openDmCreate();
    void dmCreateAction(const std::string& command);
    void drawDmCreate(Node& root, float w, float h);
    void pinDm(const std::string& id, bool pin);
    void demoDmComposeStep();
    void drawDmHome(Node& root, float width, float height);
    void drawDmRequestPanel(Node& root, float x, float y, float width, float height);
    void dmButton(Node& parent, Hit hit, const std::string& text, bool primary = false);
    bool dmHomeEvent(const api::Event& event);
    bool dmInteractionResult(const api::Event& event);
    void prepareDmHomeDemo(const std::string& view);
    void demoDmHomeStep();
    void prepareCosmeticsDemo(const std::string& view);
    void demoCosmeticsStep();
    void selectChannel(std::string id);
    void clearTyping();
    void typingEvent(const api::Event& event);
    void armTypingExpiry();
    std::string typingText() const;
    bool userTyping(const std::string& user, const std::string& channel) const;
    void drawTypingDots(Node& parent, float x, float y, float size, float step, Color color);
    void drawTyping(Node& root, float x, float y, float width, const std::string& text);
    void demoTypingStep();
    void demoFloatingStep();
    MessageOptions::ChannelMention channelMention(std::string_view guild, std::string_view channel) const;
    bool channelPrivate(const std::string& guild, const api::Channel& channel) const;
    const char* channelGlyph(const std::string& guild, const api::Channel& channel) const;
    void
    channelIcon(Node& parent, float x, float y, float size, const api::Channel& channel, Color color, Color surface);
    bool openDiscordLink(const std::string& url);
    void resumeDiscordLink();
    void prepareChatLinksDemo();
    void demoChatLinksStep();
    void drawChannelLink(
        Node& root, const MessageRun& run, const std::string& message, float x, float y, float absoluteX,
        float absoluteY
    );
    void refresh();
    void loadHistory(api::HistoryDirection direction, bool retry = false);
    void applyHistory(api::Event& event);
    void demoHistoryStep();
    void demoRoleStep();
    void drawSystemNotice(Node& parent, const api::Message& message, const MessageLayout& layout, float y, float width);
    void drawMessageStrikes(
        Node& parent, const MessageLayout& message, float x, float y, Color color, bool showSpoilers = false,
        std::optional<float> baseline = {}, std::optional<float> height = {}
    );
    void demoJoinStep();
    void demoEmbedStep();
    void demoMarkdownStep();
    void autoModerationAction(const Hit& hit);
    void deleteMessages(const std::string& channel, const std::vector<std::string>& ids);
    void clearMessageLog(bool deletions = true, bool edits = true);
    void forgetMessageLog(const std::string& channel, const std::string& id, bool deletions, bool edits);
    void updateMessage(const std::string& channel, const std::string& id, const api::MessagePatch& patch);
    void logMessageEdit(const api::Message& before, const api::Message& after);
    void drawMessageDialog(Node& root, float width, float height);
    void demoDeletedStep();
    void drawProfileCard(Node& root, float width, float height);
    bool profileAction(const Hit& hit);
    void demoProfileStep();
    void prepareProfileDemo();
    float drawGuildTag(
        Node& parent, float x, float y, float width, const std::optional<api::GuildTag>& tag, bool clickable = true
    );
    float drawRoleIcon(
        Node& parent, float x, float y, const std::string& guild, const std::string& user,
        const std::optional<std::vector<std::string>>& roles = {}
    );
    float drawRoleBadge(Node& parent, float x, float y, const api::ColoredRole& role);
    void showGuildProfile(const std::string& guild);
    void drawGuildProfile(Node& root, float width, float height);
    void clearThreads();
    const api::Channel* selectedChannel() const;
    std::optional<std::uint64_t> selectedChannelPermissions() const;
    void clearChannelFeatures();
    void closeChannelPanel();
    std::optional<std::uint64_t> channelPermissionsFor(const std::string& channel) const;
    std::optional<std::uint64_t> guildPermissions() const;
    bool openChannelContext(const Hit& target, float x, float y);
    void openChannelCreate(api::Interaction action);
    bool channelMenuEntry(int id);
    void openChannelSubmenu(int id, float center);
    bool channelMenuAction(const Hit& hit);
    void drawSidebarDialog(Node& root, float width, float height);
    void drawChannelCreate(Node& root, float width, float height);
    bool channelCreateAction(const Hit& hit);
    bool channelMuted(const std::string& channel) const;
    void openChannelInvite(const api::Interaction& action);
    void drawChannelInvite(Node& root, float width, float height);
    bool channelInviteAction(const Hit& hit);
    bool channelInviteKey(const Key& key);
    bool channelInviteResult(const api::Event& event);
    void generateChannelInvite();
    void prepareChannelInviteDemo(const std::string& view);
    void demoChannelInviteStep();
    void demoChannelCreateStep();
    bool sidebarInteraction(const api::Event& event, bool dialog);
    void submitSidebarDialog();
    void scheduleMuteExpiry();
    void demoChannelMenuStep();
    void prepareChannelMenuDemo(const std::string& view);
    void drawChannelHeader(Node& root, float width);
    float channelIntro(Node* root, float x, float y, float width);
    float messageButtons(Node* root, const api::Message& message, float x, float y, float width);
    void channelFeatureEvent(const api::Event& event);
    void componentResult(const api::Event& event);
    bool channelFeatureAction(const Hit& hit);
    bool channelFeatureInteraction(const api::Event& event, bool dialog);
    void drawChannelDialog(Node& root, float width, float height);
    void drawApplicationModal(Node& root, float width, float height);
    void submitApplicationModal();
    void dismissEphemeral(const std::string& message);
    void removeGuildChannels(const api::Event& event);
    void filterDeletedChannels(api::Event& event) const;
    void drawChannelPanel(Node& root, float x, float y, float width, float height);
    void openChannelDialog(const std::string& kind, const std::string& channel = {});
    bool channelEditClose();
    void requestChannelPage(bool next = false, bool previous = false);
    void searchChannel();
    std::vector<api::Account> searchPeople() const;
    std::vector<api::Channel> searchChannels() const;
    api::MessageSearch resolvedMessageSearch() const;
    std::vector<TextReplacement> searchReplacements(std::string_view value) const;
    void updateSearchMenu();
    void drawSearchMenu(Node& root, float width, float height);
    void closeSearchMenu(bool dismiss = false);
    bool searchMenuClick(float x, float y);
    bool searchMenuKey(const Key& key);
    void chooseSearchSuggestion(std::size_t index);
    void rememberSearch();

    void demoSearchStep();
    void toggleInbox();
    void closeInbox();
    void clearInbox();
    bool inboxAction(const Hit& hit);
    bool inboxClick(float x, float y);
    bool inboxKey(const Key& key);
    void drawInbox(Node& root, float width, float height);
    void drawInboxButton(Node& root, float width);
    void inboxEvent(const api::Event& event);
    void requestInbox(bool more = false);
    void updateInboxPreviews();
    void scheduleInboxReminders();
    void demoInboxStep();
    static constexpr int inboxReplayFrames = 24;

    static constexpr int searchReplayFrames = 12;
    void jumpToMessage(const std::string& channel, const std::string& message);
    void prepareChannelDemo(const std::string& view);
    void demoChannelStep();
    void loadThreads(bool retry = false, bool more = false);
    void rebuildThreadChannels();
    void threadEvent(const api::Event& event);
    void showThreads(const std::string& parent);
    void loadArchivedThreads(bool more = false);
    void drawThreads(Node& root, float width, float height);
    bool forumView() const;
    void drawForum(Node& root, float x, float y, float width, float height);
    void updateForumSearch();
    void newForumPost();
    void prepareForumDemo(const std::string& view);
    void demoForumStep();
    void drawForumPostDialog(Node& root, float width, float height);
    bool threadAction(const Hit& hit);
    bool threadInteraction(const api::Event& event, bool dialog);
    void openThread(const std::string& id);
    void createThread(const std::string& parent, const std::string& message = {});
    float threadStrip(Node* root, const api::Message& message, float x, float y, float width);
    void prepareThreadDemo(const std::string& view);
    void demoThreadStep();
    bool canSendToThread();
    bool guildProfileAction(const Hit& hit);
    void prepareGuildTagDemo();
    void demoGuildTagStep();
    void send();
    void rectangle(Node& parent, float x, float y, float w, float h, Color fill, float radius = 0);
    void floatingPanel(Node& parent, const layout::Frame& frame, Color fill);
    float countBadge(
        Node& parent, float right, float y, int count, bool rail = false, std::string_view noun = "unread mention"
    );
    float roleLabel(
        Node& parent, float x, float y, float width, const std::string& text, float size, Color fallback,
        FontWeight weight, const std::string& user, const std::string& key, bool hovered,
        const std::optional<std::vector<std::string>>& roles = {}
    );
    float label(
        Node& parent, float x, float y, float width, std::string text, float size, Color color,
        FontWeight weight = FontWeight::Normal, int lines = 1, bool markup = false
    );
    void button(
        Node& parent, Hit hit, std::string text, bool primary = false, bool selected = false, bool fill = false,
        bool centered = false
    );
    bool image(Node& parent, float x, float y, float size, const std::string& path, float radius);
    bool emojiImage(Node& parent, float x, float y, float size, const std::string& path);
    void avatar(
        Node& parent, float x, float y, float size, const std::string& path, const std::string& name,
        const std::string& user = {}, const std::string& decoration = {}, std::optional<layout::Frame> hoverRegion = {}
    );
    enum class NameplatePlacement { Conversation, Member, Account };
    void drawNameplate(
        Node& parent, const layout::Frame& frame, const std::string& user, const std::string& guild,
        NameplatePlacement placement = NameplatePlacement::Conversation, bool selected = false
    );
    void drawProfileCosmetics(
        Node& parent, const layout::Frame& frame, const api::UserProfile& profile, bool foreground,
        float radius = Style::radiusXl
    );
    void releaseDisabledCosmetics();
    bool cosmeticHovered(const Node& parent, const layout::Frame& region) const;
    void drawAvatarDecoration(
        Node& parent, float x, float y, float size, const std::string& path, const layout::Frame& hoverRegion
    );
    void demoCosmeticsHoverStep();
    struct CosmeticClock {
      std::uint64_t frame = 0;
      std::chrono::steady_clock::time_point start{};
    };
    std::map<std::string, CosmeticClock> m_cosmeticClocks;
    bool picture(
        Node& parent, float x, float y, float width, float height, const std::string& path, float radius,
        bool placeholder = false, bool videoPoster = false, ImageFitMode fit = ImageFitMode::Cover,
        std::optional<std::chrono::milliseconds> animationTime = {}, ImageUse use = ImageUse::Content
    );
    void glyph(Node& parent, float x, float y, float size, const std::string& name, Color color);
    void iconButton(Node& parent, Hit hit, const std::string& icon, const std::string& tooltip, bool selected = false);
    void drawEmojiPicker(Node& root, float w, float h);
    void prepareEmojiPicker();
    void closeEmojiPicker();
    void clearEmojiState();
    void emojiEvent(api::Event& event);
    void pickEmoji(const std::string& key);
    void recordEmojiUse(const std::string& key);
    void favoriteEmoji(const std::string& key);
    void prepareStickers();
    void clearStickers();
    void stickerEvent(api::Event& event);
    void drawStickerPicker(Node& root, float w, float h);
    void stickerAction(const Hit& hit);
    void demoStickerStep();
    std::string stickerRestriction(const api::Sticker& sticker) const;
    std::string favoriteEmojiKey(const api::Emoji& emoji) const;
    void drawGifPicker(Node& root, float w, float h);
    void searchGifs();
    void closeGifPicker();
    void clearGifs();
    void gifEvent(api::Event& event);
    void toggleGifFavorite(const api::Gif& gif);
    void demoGifStep();
    void drawTooltip(Node& root, float w, float h);
    float drawGuildBadge(Node& root, float x, float y, api::GuildBadge badge, bool tooltip = true);
    void drawGuildTooltip(Node& root, float w, float h);
    void demoGuildHoverStep();
    void drawSettings(Node& root, float w, float h);
    void verifySettingsScrollbarGutters() const;
    void notifyMessage(const api::Message& message, const std::string& channel);
    void drainDesktopNotifications();
    void desktopNoticeAction(DesktopNoticeAction action);
    bool extendedPreference(const std::string& action);
    std::string shortcutLabel(const std::string& action) const;
    bool shortcutKey(const Key& key);
    void demoUserPreferencesStep();
    static constexpr int reducedMotionReplayFrames = 12;
    void demoReducedMotionStep();
    void demoDecorationsStep();
    void drawTitlebar(Node& root, float width);
    void drawWindowIcon(Node& node, float x, float y, WindowIcon icon, bool over);
    void drawWindowControlSettings(UserSettingsForm& form);
    bool windowControlPreference(const std::string& action);
    void updateWindowIcons();
    void drainWindowIcons();
    void demoWindowControlsStep();
    static constexpr unsigned windowControlsReplayFrames = 24;
    bool spoilersVisible(const std::string& message) const;
    void resetUserAccountSettings();
    void loadUserAccountSettings(const std::string& page, bool force = false);
    bool requestUserAccount(api::UserSettingsRequest request, std::string cache = {});
    bool userAccountResult(const api::Event& event);
    bool userAccountAction(const std::string& action);
    bool drawUserAccountSettings(UserSettingsForm& form);
    void drawUserAccountDialog(Node& root, float width, float height);
    bool userAccountPickedFile(PickedFile& result);
    bool userAccountDialogKey(const Key& key);
    bool userAccountDialogClick(float x, float y);
    bool userAccountDialogScroll(float delta);
    bool userAccountDialogOpen() const { return !m_userAccount.dialog.empty() || m_userAccount.mfaRequest.has_value(); }
    void closeUserAccountDialog();
    bool drawUserProfileCrop(UserSettingsForm& form);
    bool userProfileCropAction(const std::string& action);
    void beginUserProfileUpload(const std::string& field);
    void prepareUserAccountDemo();
    void demoUserAccountStep();
    void resetUserSettingsLists();
    void loadUserSettingsLists();
    bool userSettingsListsAction(const std::string& action);
    bool userSettingsListsResult(const api::Event& event);
    bool drawUserSettingsLists(UserSettingsForm& form);
    bool requestUserSettingsList(api::UserSettingsRequest request);
    void prepareUserSettingsListsDemo();
    void demoUserSettingsListsStep();
    void openServerSettings(const std::string& guild = {});
    void closeServerSettings(bool discard = false);
    bool serverSettingsAction(const Hit& hit);
    void drawServerSettings(Node& root, float width, float height);
    void serverSettingsResult(const api::Event& event);
    void serverSettingsRoleUpdate(const api::RoleUpdates& updates);
    bool serverAuditAction(const std::string& action);
    void loadServerAudit(bool more = false);
    bool serverAuditResult(const api::Event& event);
    float drawServerAudit(Node& parent, float width);
    void resetServerAudit();
    void prepareServerAuditDemo();
    bool loadServerCommands(bool more = false);
    bool serverCommandsResult(const api::Event& event);
    bool serverCommandsAction(const std::string& action);
    bool drawServerCommands(ServerSettingsForm& form);
    void resetServerCommands();
    void prepareServerCommandsDemo();
    bool drawServerExtraForms(ServerSettingsForm& form, std::string_view part = {});
    bool serverExtraAction(const std::string& action);
    bool serverExtraResult(const api::Event& event);
    void beginServerImageCrop(
        const std::string& field, const std::string& mime, std::vector<std::uint8_t> bytes, DecodedRasterImage raster
    );
    bool drawServerImageCrop(ServerSettingsForm& form);
    bool serverImageCropAction(const std::string& action);
    void prepareServerManagementDemo();
    void demoServerManagementStep();
    bool drawServerManagementForms(ServerSettingsForm& form);
    bool drawServerExpressions(ServerSettingsForm& form);
    bool serverManagementAction(const std::string& action);
    bool loadServerManagement(bool more = false);
    bool serverManagementResult(const api::Event& event);
    void playServerSound(const std::string& sound);
    void stopServerSound();
    void prepareServerSettingsDemo();
    void demoServerSettingsStep();
    void demoServerExpressionsStep();
    void demoServerCommandsStep();
    void selectServerSettingsPage(const std::string& page, bool discard = false);
    void loadServerSettings(bool more = false);
    bool requestServerSettings(api::GuildSettingsRequest request);
    void setServerSettingsValue(const std::string& field, nlohmann::json value);
    api::GuildSettingsAccess serverSettingsAccess(const std::string& resource = {}) const;
    void serverSettingsUpload(const std::string& field, const std::string& mime, std::vector<std::uint8_t> bytes);
    void startServerSettingsUpload(const std::string& field);
    void drainServerSettingsUploads();
    void stageServerEmojiUploads(std::vector<PickedFile> files);
    std::string serverSettingsScope() const;
    bool openFolderContext(const Hit& target, float x, float y);
    bool folderMenuEntry(int id);
    api::Interaction guildReadAction(const std::vector<std::string>& guilds) const;
    void demoFolderMenuStep();
    bool openServerContext(const Hit& target, float x, float y);
    std::optional<std::uint64_t> serverGuildPermissions(const std::string& guild) const;
    const std::vector<api::Channel>& serverChannels(const std::string& guild) const;
    const api::Channel* serverChannel(const std::string& guild, const std::string& channel) const;
    std::optional<std::uint64_t> serverChannelPermissions(const std::string& guild, const std::string& channel) const;
    std::vector<std::pair<std::string, std::string>> serverSettingsPages(const std::string& guild) const;
    bool serverMenuEntry(int id);
    void openServerSubmenu(int id, float center);
    bool serverMenuInteractionResult(const api::Event& event);
    void resumeServerInvite(const api::Event& event);
    void openServerMenuDialog(const std::string& kind, const std::string& guild);
    void drawServerMenuDialog(Node& root, float width, float height);
    void submitServerMenuDialog();
    void serverMenuDialogAction(const std::string& id);
    bool serverMenuDialogResult(const api::Event& event);
    void openServerProfile(const std::string& guild);
    bool serverChannelVisible(const api::Channel& channel) const;
    void demoServerMenuStep();
    Input& inputControl(
        Node& parent, float x, float y, float width, float height, const std::string& id, const std::string& value,
        const std::string& placeholder = {}, bool multiline = false, bool password = false
    );
    void beginControls();
    void finishControls();
    void openSelectDropdown(const DropdownRequest&, DropdownCallbacks) override;
    void closeSelectDropdown() override;
    bool isSelectDropdownOpen() const override { return m_selectRequest.has_value(); }
    void drawSelectDropdown(Node&, float, float);
    bool selectDropdownClick(float, float);
    bool selectDropdownKey(const Key&);
    void selectDropdownChoose(std::size_t);
    void selectDropdownReveal();
    void focusControl(const std::string& id);
    void pasteControl(const std::string& id, std::string text);
    void drawPresenceMenu(Node& root, float h);
    void openSettings();
    bool settingsVisible() const { return m_settingsOpen || m_serverSettings.open; }
    void selectSettingsPage(const std::string& page);
    void drawVoiceSettings(UserSettingsForm& form);
    void drawVoiceBar(Node& root, float windowHeight);
    void voiceAction(const std::string& action);
    void configureVoice();
    void voiceTick();
    void releasePushToTalk();
    bool voiceBarVisible() const;
    std::size_t voiceMemberCount(const api::Channel& channel) const;
    float voiceMembersHeight(const api::Channel& channel) const;
    void drawVoiceMembers(Node& root, const api::Channel& channel, float y, float top, float bottom);
    void prepareVoiceDemo(const std::string& view);
    void demoVoiceStep();
    void clearAccountSettings();
    void resetProfile();
    void settingsKey(const Key& key);
    bool fontPreference(const std::string& id);
    bool fontPickerClick(float x, float y);
    void drawFontPicker(Node& root, float w, float h);
    void settingsScrollbar(Node& parent, const std::string& id, Hit bounds, float content, float& offset);
    void demoFontStep();
    void demoThemeStep();
    void savePreferences();
    void setPresence(const std::string& status, bool custom = false);
    void drawImageViewer(Node& root, float w, float h);
    void changeImage(int direction);
    struct DraftImage;
    const DraftImage* draftImage(std::string_view path) const;
    void openDraftAttachment(const std::string& key);
    void prepareAttachmentsDemo();
    void demoAttachmentsStep();
    void prepareMediaGalleryDemo(int count, bool portrait = false);
    void demoMediaGalleryStep();
    void openImageContext(float x, float y);
    void imageViewerAction(Action action);
    std::string viewerMediaLink() const;
    void closeImageViewer();
    void enterMediaViewer();
    bool imageSaved(const PickedFile& result);
    void demoImageMenuStep();
    bool hovered(float x, float y, float width, float height) const;
    void updateCursor();
    float channelSidebarWidth() const;
    float chatLeft() const;
    float channelPanelWidth() const;
    bool sidebarResizeAvailable() const;
    bool sidebarResizeAt(float x, float y) const;
    void finishSidebarResize();
    void sidebarDemoFrame();
    void positionCaret(float x, float y, bool extend);
    void positionChatSelection(float x, float y);
    void upsertMessage(api::Message message);
    void saveSelection();
    std::string userName(const std::string& id, const std::string& fallback) const;
    std::string userStatus(const std::string& id, const std::string& fallback = "offline") const;
    std::string channelName(const api::Channel& channel) const;
    void updateDmMembers();
    const api::Account* dmRecipient() const;
    float memberPaneWidth() const;
    void refreshDmProfile(bool retry = false);
    bool dmProfileResult(const api::Event& event);
    void drawDmProfile(Node& root, const layout::Frame& frame);
    void drawCustomStatus(Node& root, const layout::Frame& frame, const api::UserCustomStatus& status, float size);
    void prepareDmProfileDemo(const std::string& view);
    void demoDmProfileStep();
    struct DmProfileState {
      std::string user, channel, error;
      std::optional<api::UserProfile> data;
      std::optional<api::Interaction> request;
      float scroll = 0;
    };
    DmProfileState m_dmProfile;
    std::uint64_t m_dmProfileSerial = 0;
    void statusBadge(
        Node& parent, float x, float y, const std::string& status, Color backdrop, const std::string& user = {},
        const std::string& channel = {}
    );
    void openContext(float x, float y);
    void closeContext();
    void drawContext(Node& root, float w, float h);
    void contextAction(Action action);
    bool overlayKey(const Key& key);
    bool overlayClick(float x, float y);
    api::Account lookupUser(const std::string& id) const;
    bool startInteraction(api::Interaction action);
    void interactionResult(const api::Event& event);
    void showProfile(const std::string& user);
    void drawDialog(Node& root, float w, float h);
    void submitDialog();
    void closeDialog();
    float
    reactionStrip(Node* parent, const api::Message& message, float x, float y, float width, bool addButton = false);
    bool blockedMessage(const api::Message& message) const;
    bool interactionAction(const Hit& hit);
    void clearInteractions();
    void contextMotion(float x, float y);
    void scrollContext(float delta);
    void chooseReaction(const std::string& emoji);
    void openReactionSubmenu(float rowCenter);
    void hoverReaction(const api::Message& message, const api::Reaction& reaction, const Hit& chip);
    void requestReactors(api::Interaction action);
    void invalidateReactors(const std::string& channel, const std::string& message);
    void demoInteractionStep();

    SingleInstance* m_instance = nullptr;
    Window m_window;
    WindowIcons m_windowIcons;
    std::optional<WindowIconRequest> m_windowIconRequest;
    std::uint64_t m_windowIconGeneration = 0;
    std::array<TextureHandle, windowIconNames.size()> m_windowIconTextures;
    std::array<std::string, windowIconNames.size()> m_windowIconErrors;
    ScrollMotion m_scrolling;
    api::Client m_client;
    voice::State m_voiceState;
    api::VoiceRoster m_voiceRoster;
    std::string m_voiceGuild;
    voice::AudioState m_voiceAudioState;
    Timer m_voiceJoinTimer;
    bool m_voiceMuted = false, m_voiceDeafened = false, m_voicePtt = false;
    std::uint32_t m_voicePttKey = 0;
    DesktopTray m_tray;
    bool m_trayAvailable = false, m_restartRequested = false;
    Timer m_trayStartupTimer;
    Timer m_trayDemoTimer;
    unsigned m_trayHiddenTicks = 0;
    api::Assets m_assets;
    media::VideoPlayer m_video;
    media::VideoPlayer m_soundPreview;
    std::string m_soundPreviewSource;
    bool m_soundPlayRequested = false;
    EGLContext m_videoContext = EGL_NO_CONTEXT;
    GlesFramebuffer m_videoTarget;
    std::string m_videoSource;
    bool m_videoExpanded = false, m_videoVisible = false, m_videoWasPlaying = false, m_videoPlayRequested = false;
    bool m_videoPreviousFullscreen = false;
    int m_videoDrag = 0;
    float m_videoScrub = 0;
    Hit m_videoSeek{}, m_videoVolume{}, m_videoBounds{};
    std::map<std::string, std::string> m_videoPosters;
    auth::BrowserLogin m_login;
    SessionStore m_sessionStore;
    ThemeRuntime m_themes;
    FilePicker m_filePicker;
    ServerSettingsState m_serverSettings;
    ServerAuditState m_serverAudit;
    ServerCommandsState m_serverCommands;
    std::uint64_t m_serverManagementSession = 0;
    std::map<std::string, nlohmann::json> m_serverManagementJobs;
    std::optional<DropdownRequest> m_selectRequest;
    DropdownCallbacks m_selectCallbacks;
    std::string m_selectOwner;
    ContextMenuControl* m_selectControl = nullptr;
    Hit m_selectBounds{};
    float m_selectScroll = 0, m_selectContent = 0;
    bool m_selectDirty = false;
    std::string m_serverUploadField, m_serverUploadGuild, m_serverUploadPage, m_serverUploadResource;
    std::uint64_t m_serverUploadSerial = 0, m_serverUploadGeneration = 0, m_serverUploadSession = 0;
    ThemeSnapshot m_themeSnapshot;
    ChannelListPreferences m_channelListPreferences;
    std::vector<ChannelMenuCommand> m_channelMenuCommands;
    std::vector<ServerMenuCommand> m_serverMenuCommands;
    api::GuildNotification m_serverNotificationDraft;
    api::GuildNotification m_serverNotificationOriginal;
    api::GuildPrivacy m_serverPrivacyOriginal;
    api::GuildPrivacy m_serverPrivacyDraft;
    bool m_serverPrivacyLoaded = false;
    std::string m_serverDialogGuild, m_serverDialogFolder, m_serverFolderColor;
    std::string m_serverInvitePending;
    std::uint64_t m_guildLayoutRevision = 0;
    float m_serverDialogScroll = 0;
    std::vector<ContextMenuControlEntry> m_channelSubmenuEntries;
    bool m_channelSubmenu = false;
    std::string m_sidebarInviteUrl;
    ChannelInviteState m_channelInvite;
    std::map<std::string, api::Interaction> m_channelInviteSettings;
    bool m_channelCreateAccessStep = false;
    std::string m_channelCreateFilter;
    float m_channelCreateScroll = 0;
    Timer m_channelMuteTimer;
    float m_contextScroll = 0, m_contextFullHeight = 0;
    std::map<std::string, api::GuildUpdate> m_guildUpdates;
    Hit m_guildTooltipBounds{};
    std::string m_restoreChannel;
    std::string m_dmPage, m_dmFilter = "online", m_dmSearch, m_dmFriendName, m_dmStatus, m_dmConfirm;
    std::string m_dmCreateSearch, m_dmCreateError;
    std::vector<api::Account> m_dmCreateCandidates;
    std::vector<std::string> m_dmCreateRecipients;
    std::uint64_t m_dmCreateRequest = 0;
    float m_dmCreateScroll = 0;
    Hit m_dmCreateBounds{};
    std::map<std::string, std::uint64_t> m_dmPinRequests;
    std::string m_dmPreviewId;
    std::vector<api::Message> m_dmPreview;
    std::set<std::string> m_dmPending;
    float m_dmHomeScroll = 0, m_dmPreviewScroll = 0;
    Hit m_dmPreviewBounds{};
    std::uint64_t m_dmSerial = 0, m_dmPreviewRequest = 0, m_dmFriendRequest = 0;
    bool m_dmPreviewLoading = false;
    bool m_privateReady = false;
    std::optional<api::Interaction> m_dmDemoAction;
    DesktopNotifications m_desktopNotifications;
    std::vector<std::string> m_notifiedMessages;
    Preferences m_preferences;
    UserAccountState m_userAccount;
    UserSettingsListsState m_userSettingsLists;
    std::string m_shortcutCapture;

    api::RoleDirectory m_roleDirectory;
    RoleMotion m_roleMotion;
    api::Presence m_presence;
    bool m_presenceMenu = false, m_settingsOpen = false, m_savingProfile = false, m_loadingProfile = false,
         m_profileDirty = false;
    std::string m_settingsPage = "cressida", m_settingsField, m_settingsStatus, m_desiredPresence;
    std::map<std::string, std::string> m_settingsFields;
    std::vector<std::string> m_installedFonts;
    std::vector<std::string> m_fontMatches;
    std::string m_fontFilter;
    bool m_fontPickerOpen = false;
    float m_fontPickerReturnScroll = 0;
    float m_fontPickerScroll = 0;
    Hit m_fontPickerAnchor{}, m_fontPickerBounds{};
    float m_settingsScroll = 0, m_settingsNavScroll = 0, m_settingsNavRight = 0;
    api::SettingValues m_accountSettings, m_settingsDraft;
    bool m_loadingSettings = false, m_savingSettings = false, m_settingsLoaded = false;
    std::map<std::string, std::vector<api::SettingsEntry>> m_settingsLists;
    std::map<std::string, std::string> m_settingsListErrors;
    std::set<std::string> m_loadingSettingsLists;
    std::unique_ptr<Node> m_scene;
    std::map<std::string, Input*> m_inputs;
    std::set<std::string> m_multilineInputs;
    std::map<std::string, Node*> m_widgets;
    std::map<std::string, std::unique_ptr<Node>> m_detachedInputs;
    InputDispatcher m_controlInput;
    std::uint64_t m_controlFontGeneration = 0;
    std::unique_ptr<TextClipboard> m_controlClipboard;
    std::string m_focusedInput, m_pasteInput, m_pasteServerScope;
    bool m_buildingScene = false, m_emojiMenu = false;
    EmojiCatalog m_emojiCatalog;
    api::EmojiPreferences m_emojiPreferences;
    std::vector<std::string> m_recentEmojis;
    std::set<std::string> m_loadedEmojiGuilds, m_loadingEmojiGuilds, m_pendingEmojiFavorites;
    std::map<std::string, std::string> m_emojiGuildErrors;
    bool m_emojiPreferencesLoaded = false, m_loadingEmojiPreferences = false, m_emojiPreferencesDirty = false,
         m_emojiOpened = false;
    std::string m_emojiQuery, m_emojiSection = "all", m_emojiError, m_emojiHovered, m_emojiJump;
    Hit m_emojiBounds{};
    float m_emojiScroll = 0, m_emojiRailScroll = 0;
    bool m_stickerTab = false, m_stickerInfo = false;
    std::map<std::string, std::vector<api::Sticker>> m_stickers;
    std::set<std::string> m_loadedStickerSources, m_loadingStickerSources;
    std::map<std::string, std::string> m_stickerErrors;
    std::vector<std::pair<std::string, std::vector<api::Sticker>>> m_stickerGroups;
    std::vector<std::string> m_recentStickers;
    std::string m_stickerSection = "all", m_stickerFilter;
    std::map<std::string, std::vector<api::Sticker>> m_draftStickers;
    std::map<std::string, api::Sticker> m_stickerTargets;
    std::optional<api::Sticker> m_inspectedSticker;
    std::uint64_t m_stickerRevision = 0;
    bool m_gifMenu = false, m_gifFavoritesTab = false, m_loadingGifs = false, m_loadingGifFavorites = false,
         m_gifFavoritesLoaded = false, m_gifFavoritesDirty = false;
    std::string m_gifQuery, m_gifRequestedQuery, m_gifError, m_gifFavoriteError;
    std::vector<api::Gif> m_gifs, m_gifFavorites;
    std::map<std::string, api::Gif> m_gifTargets;
    std::set<std::string> m_pendingGifFavorites;
    Hit m_gifBounds{};
    float m_gifScroll = 0, m_gifMaxScroll = 0;
    Timer m_gifSearchTimer;
    std::string m_tooltip;
    std::vector<Hit> m_contextTargets;
    std::vector<ContextMenuControlEntry> m_contextEntries;
    ContextMenuControl* m_contextControl = nullptr;
    ContextMenuControl* m_reactionControl = nullptr;
    std::vector<std::string> m_frequentReactions{"❤️", "👍", "😂", "🔥"};
    std::vector<std::pair<std::string, std::string>> m_reactionChoices;
    bool m_reactionSubmenu = false, m_reactionSubmenuDirty = false, m_reactionSubmenuFocus = false;
    float m_reactionSubmenuAnchor = 0, m_reactionSubmenuScroll = 0, m_reactionSubmenuHeight = 0;
    Hit m_reactionSubmenuBounds{};
    bool m_contextOpen = false, m_contextDirty = false;
    int m_contextQuickIndex = -1;
    float m_contextX = 0, m_contextY = 0;
    Hit m_contextBounds{}, m_dialogBounds{};
    std::string m_contextMessage, m_contextUser, m_contextChannel;
    std::string m_contextGuild;
    std::string m_dialogKind, m_dialogTitle, m_dialogText, m_dialogError;
    std::optional<api::Interaction> m_dialogAction;
    struct MessageEditState {
      std::string channel, message, text, error;
      std::optional<api::Interaction> request;
      std::uint64_t generation = 0;
    };
    std::optional<MessageEditState> m_messageEdit;
    std::uint64_t m_messageEditGeneration = 0, m_pasteMessageEditGeneration = 0;
    struct CompletionItem {
      enum Kind { Member, Role, Emoji, Broadcast, Channel } kind;
      std::string id, name, detail, insert, image;
      int score = 0;
      std::uint32_t color = 0;
      std::string glyph;
    };
    std::optional<CompletionQuery> m_completionQuery;
    std::vector<CompletionItem> m_completionItems;
    std::string m_completionInput, m_completionSnapshot, m_completionDismissed;
    std::string m_completionNonce, m_completionGuild, m_completionSearchError;
    std::vector<api::Account> m_completionMembers;
    std::uint64_t m_completionSerial = 0;
    bool m_completionDirty = true;
    std::size_t m_completionSelected = 0;
    float m_completionScroll = 0, m_completionViewport = 0;
    Hit m_completionBounds{};
    Timer m_completionSearchTimer, m_completionTimeout;
    api::Account m_dialogAccount;
    api::UserProfile m_profileCard;
    api::GuildProfile m_guildProfile;
    bool m_guildPreviewJoined = false, m_guildPreviewVerification = false;
    bool m_profileExpanded = false, m_profileRevealSpoilers = false;
    float m_profileScroll = 0, m_profileAnchorX = -1, m_profileAnchorY = -1;
    bool m_dialogPending = false;
    std::uint64_t m_dialogGeneration = 0, m_pasteDialogGeneration = 0;
    std::set<std::string> m_pendingInteractions, m_revealedBlocked;
    std::map<std::string, api::MessageReference> m_replies;
    std::string m_reactionMessage, m_reactionChannel;
    struct ReactorPage {
      std::vector<api::Account> users;
      std::string error;
      bool pending = false, loaded = false;
      std::uint64_t revision = 0;
      std::chrono::steady_clock::time_point touched{};
    };
    std::map<std::string, ReactorPage> m_reactorPages;
    std::uint64_t m_reactorSerial = 0;
    std::string m_hoverReactionKey;
    Timer m_reactorHoverTimer;
    bool m_reactionHoveredThisFrame = false;
    std::map<std::string, float> m_messageTops;
    std::vector<layout::Frame> m_floatingFrames;
    float m_tooltipX = 0, m_tooltipY = 0, m_composerTop = 0;
    bool m_demo = false, m_demoPrepared = false;
    bool m_traceScroll = false, m_animateWheel = true;
    std::string m_demoOverlay;
    bool m_demoOverlayApplied = false;
    unsigned m_demoInteractionStep = 0;
    std::optional<api::HistoryQuery> m_demoHistoryQuery;
    std::optional<TimelineAnchor::Row> m_demoHistoryAnchor;
    std::optional<TimelineAnchor::Row> m_demoMotionAnchor;
    struct TypingLayoutSnapshot {
      std::string channel;
      float composerTop, viewportTop, viewportBottom, maximumScroll, scroll;
      std::map<std::string, float> messageTops;
    };
    std::optional<TypingLayoutSnapshot> m_demoTypingLayout;
    unsigned m_demoHistoryPages = 0, m_demoHistoryPhase = 0, m_demoHistoryMotion = 0;
    bool m_connected = false;
    bool m_connecting = false, m_restoringLogin = false, m_retrySavedLogin = false;
    std::string m_loginStorageStatus;
    bool m_showTokenEntry = false;
    bool m_loadingChannels = false;
    bool m_loadingMessages = false;
    MessageHistory m_history;
    TimelineAnchor m_timelineAnchor;
    bool m_keepHistoryAnchor = false;
    bool m_sending = false;
    bool m_live = false;
    bool m_showMembers = true;
    bool m_dmMode = false;
    bool m_loadingPrivate = false;
    std::string m_gatewayStatus = "Connecting…";
    bool m_fieldFocused = true;
    bool m_selectAll = false;
    bool m_pasteConnected = false;
    std::uint64_t m_pasteSession = 0;
    std::string m_pasteChannel;
    std::uint64_t m_session = 0;
    api::Account m_account;
    api::UserDirectory m_userDirectory;
    std::vector<api::Guild> m_guilds;
    std::vector<api::Channel> m_channels;
    std::vector<api::Channel> m_privateChannels;
    std::set<std::string> m_collapsedCategories;
    std::set<std::string> m_unreadDMs, m_unreadChannels;
    std::map<std::string, std::string> m_unreadGuilds, m_localRead;
    UnreadTimeline m_unreadTimeline;
    std::map<std::string, std::vector<api::Channel>> m_guildChannels;
    api::ThreadDirectory m_threads;
    std::map<std::string, api::ThreadQuery> m_threadRequests;
    std::map<std::string, api::ThreadPage> m_threadPages;
    std::map<std::string, api::Message> m_forumFirstMessages;
    std::string m_forumSearch, m_forumTag, m_forumPostTag;
    bool m_forumNewest = false;
    float m_forumScroll = 0;
    Timer m_forumSearchTimer;
    std::set<std::string> m_threadsLoaded, m_threadPending;
    std::map<std::string, std::string> m_threadErrors;
    std::optional<api::ThreadQuery> m_archiveQuery, m_openThreadQuery;
    std::string m_threadParent, m_threadBefore, m_threadError;
    api::ThreadList m_threadTab = api::ThreadList::Active;
    bool m_threadMore = false;
    float m_threadScroll = 0;
    std::uint64_t m_threadSerial = 0;
    std::set<std::string> m_channelDetailRequests, m_channelDetailsLoaded;
    std::map<std::string, std::string> m_channelDetailErrors;
    std::string m_channelSearchText, m_channelPanel, m_channelPanelError;
    struct SearchSuggestion {
      std::string title, detail, insert, glyph = "search", avatar;
      bool history = false, more = false;
    };
    std::vector<SearchSuggestion> m_searchSuggestions;
    std::vector<std::pair<std::string, std::string>> m_searchHistory;
    std::string m_searchInput, m_searchSnapshot, m_searchIdentity, m_searchDismissed;
    std::optional<std::size_t> m_searchHistoryStart;
    std::size_t m_searchReplaceBegin = 0, m_searchReplaceEnd = 0;
    int m_searchSelected = -1;
    bool m_searchMore = false;
    Hit m_searchBounds{};
    float m_searchScroll = 0, m_searchViewport = 0;

    struct InboxTab {
      api::InboxPage page;
      std::optional<api::InboxQuery> pending;
      std::string error;
      bool loaded = false;
      std::uint64_t revision = 0;
    };
    struct InboxPreview {
      std::string latest, after, error;
      std::vector<api::Message> messages;
      std::optional<api::InboxQuery> pending;
    };
    bool m_inboxOpen = false, m_inboxFilters = false, m_inboxMuted = false, m_inboxGuild = false;
    bool m_inboxRoles = true, m_inboxEveryone = true, m_inboxBootstrap = false;
    int m_inboxTab = 0;
    std::array<InboxTab, 3> m_inboxTabs;
    std::map<std::string, InboxPreview> m_inboxPreviews;
    std::vector<std::pair<std::string, std::string>> m_inboxUnread;
    std::set<std::string> m_inboxVisibleUnread;
    std::map<std::uint64_t, api::InboxQuery> m_inboxWrites;
    std::uint64_t m_inboxSerial = 0, m_inboxRevision = 0;
    std::set<std::string> m_inboxNotified;
    Timer m_inboxReminderTimer;
    std::string m_inboxStatus, m_inboxRemindTarget;
    Hit m_inboxBounds{};
    float m_inboxScroll = 0, m_inboxViewport = 0;
    std::size_t m_inboxUnreadPage = 0;
    std::optional<api::InboxQuery> m_inboxDemoWrite;
    std::optional<api::ChannelPageQuery> m_channelPageRequest, m_channelPageShown;

    api::ChannelPageResult m_channelPage;
    std::vector<std::string> m_pinCursors;
    float m_channelPanelScroll = 0, m_channelDialogScroll = 0;
    std::string m_channelFormName, m_channelFormTopic, m_channelAccessFilter, m_channelAccessTarget;
    int m_channelAccessMode = 0;
    api::Channel m_channelEditOriginal;
    std::string m_channelEditPage = "overview";
    bool m_channelFormNsfw = false;
    int m_channelFormRateLimit = 0;
    std::string m_channelPermissionTarget;
    int m_channelPermissionType = 0;
    std::map<std::uint64_t, int> m_channelPermissionDraft, m_channelPermissionPending;
    bool m_channelReturnToPermissions = false;
    std::uint64_t m_channelFeatureSerial = 0;
    std::string m_seekMessage;
    std::string m_jumpHighlightMessage;
    Timer m_typingTimer;
    TypingIndicator m_typing;
    bool m_typingVisible = false;
    Timer m_jumpHighlightTimer;
    std::optional<std::pair<std::string, std::string>> m_pendingMessageJump;
    std::optional<DiscordLink> m_pendingDiscordLink;
    struct ComponentState {
      std::string nonce, status;
      bool pending = false;
    };
    std::map<std::string, ComponentState> m_componentStates;
    std::optional<api::ApplicationModal> m_applicationModal;
    std::vector<std::string> m_applicationValues;
    std::string m_modalNonce;
    std::set<std::string> m_dismissedEphemeral;
    std::set<std::string> m_deletedChannels;
    std::map<std::string, std::vector<api::Message>> m_channelMessages;
    std::map<std::string, ChatCacheState> m_channelHistoryState;
    std::uint64_t m_chatCacheEpoch = 0;
    std::uint64_t m_historyEpoch = 0, m_chatPreloadEpoch = 0;
    std::vector<std::string> m_channelCacheOrder;
    struct MemberSnapshot {
      std::vector<api::MemberRow> rows;
      int count = 0;
      std::string identity;
      std::uint64_t epoch = 0;
    };
    std::map<std::string, MemberSnapshot> m_channelMembers;
    std::string m_activeMemberIdentity;
    std::uint64_t m_memberEpoch = 0;
    std::map<std::string, std::string> m_lastChannels;
    void cacheChannel();
    void touchChannelCache(const std::string& id);
    bool restoreCachedHistory(const std::string& id);
    std::optional<api::MemberListIdentity> channelMemberIdentity(const std::string& id) const;
    void refreshMemberSubscription();
    bool adoptChatPreload(const std::string& id);
    bool canPreloadChat(const std::string& id) const;
    std::string hoveredPreloadChat() const;
    void updateChatPreload();
    void startChatPreload();
    void clearChatPreload();
    bool chatPreloadEvent(api::Event& event);
    void demoChatPreloadStep();
    Timer m_chatPreloadTimer;
    std::string m_chatPreloadHover, m_chatPreloadChannel;
    MessageHistory m_chatPreloadHistory;
    std::map<std::string, std::optional<api::Message>> m_chatPreloadLive;
    std::vector<api::ReactionUpdate> m_chatPreloadReactions;
    std::chrono::steady_clock::time_point m_chatPreloadAfter{};
    struct AdoptedChatPreload {
      std::string channel;
      api::HistoryQuery source, visible;
      std::vector<api::ReactionUpdate> reactions;
    };
    std::optional<AdoptedChatPreload> m_adoptedChatPreload;
    bool readingChannel(const std::string& channel) const;
    bool markChannelRead(bool explicitAction = false);
    bool acknowledgeChannelRead(const std::string& channel, const std::string& message, bool automatic = false);
    void dispatchReadAcknowledgements();
    void cancelAutomaticRead(const std::string& channel);
    void resetReadAcknowledgements();
    void failedReadAcknowledgement(const std::string& channel, const std::string& message);
    Timer m_readAcknowledgementTimer;
    ReadAcknowledgements m_readAcknowledgements;
    std::vector<api::Message> m_messages;
    MessageLog m_messageLog;
    struct CachedMessageLayout {
      MessageLayout layout;
      MessageLayout reply;
      std::optional<api::MessageReference> replySource;
      float width = -1, scale = -1;
      std::uint64_t fonts = 0;
    };
    std::map<std::string, CachedMessageLayout> m_messageLayouts;
    std::vector<api::GuildFolder> m_folders;
    api::GuildLayout m_guildLayout;
    bool m_gatewayGuildsReady = false, m_gatewayLayoutReady = false;
    bool m_readyBootstrapPending = false;
    std::string m_startupGuild;
    std::set<std::string> m_openFolders;
    FolderMotion m_folderMotion;
    bool m_foldersRestored = false;
    std::vector<api::MemberRow> m_members;
    int m_memberCount = 0;
    int m_memberPage = 0;
    std::map<std::string, std::optional<api::Message>> m_liveChanges;
    void discardImage(const std::string& path);
    void discardImagesWithPrefix(std::string_view prefix);
    ImageCache m_images;
    std::vector<TextureHandle> m_retiredImages;
    std::vector<std::uint8_t> m_animationPixels;
    std::map<std::string, ImageUses> m_pendingImages;
    std::uint64_t m_imageSerial = 0;
    std::uint64_t m_animationDraw = 0;
    std::chrono::milliseconds m_animationDelay = std::chrono::milliseconds::max();
    Timer m_animationTimer;
    std::string m_token;
    std::string m_guildId;
    std::string m_channelId;
    std::string m_status;
    std::map<std::string, std::string> m_drafts;
    std::map<std::string, std::map<std::string, std::string>> m_draftMentionNames;
    struct DraftImage {
      api::Upload upload;
      TextureHandle texture;
      std::string key;
    };
    std::map<std::string, std::vector<DraftImage>> m_attachments;
    struct PendingSend {
      api::Message preview;
      std::string draft;
      std::vector<DraftImage> images;
      std::vector<api::Sticker> stickers;
      std::map<std::string, std::string> mentions;
      std::string confirmed, error;
      bool completed = false, composer = false;
    };
    std::map<std::string, PendingSend> m_pendingSends;
    bool queueOutgoing(
        std::string content, std::vector<api::Upload> uploads = {}, std::vector<std::string> stickers = {},
        std::vector<api::Sticker> previews = {}, bool consumeDraft = true, std::optional<api::Gif> gif = {}
    );
    void confirmOutgoing(api::Message& message);
    void outgoingResult(api::Event& event);
    void releaseOutgoing(PendingSend& pending);
    void clearOutgoing();
    void restoreOutgoing(const std::string& nonce);
    void demoOutgoingStep();
    static constexpr int outgoingReplayFrames = 12;
    std::vector<std::string> m_demoOutgoingNonces;
    std::uint32_t m_demoOutgoingTexture = 0;
    struct ViewerImage {
      std::string path, url;
      float width = 0, height = 0;
    };
    std::vector<ViewerImage> m_viewerImages;
    std::string m_viewImage;
    std::string m_viewUrl;
    std::string m_imageContext, m_imageActionSource, m_imageActionViewer, m_imageStatus;
    Action m_pendingImageAction = Action::None;
    std::uint64_t m_imageSaveSerial = 0, m_imageSaveRequest = 0;
    float m_viewZoom = 1, m_viewPanX = 0, m_viewPanY = 0, m_imageDragX = 0, m_imageDragY = 0;
    bool m_dragImage = false;
    bool m_resizingSidebar = false;
    float m_sidebarDragOffset = 0, m_sidebarDragStart = layout::Sidebar::preferredWidth;
    std::uint32_t m_controlCursor = 1;
    TextEdit m_edit, m_chatEdit;
    bool m_dragDraft = false, m_dragMessage = false;
    std::string m_selectedMessage, m_selectedText;
    std::set<std::string> m_revealedSpoilers;
    struct TextArea {
      MessageRun run;
      std::string message;
      float x, y;
    };
    std::vector<TextArea> m_textAreas;
    std::map<std::string, std::string> m_messageText;
    std::vector<std::size_t> m_draftOffsets;
    std::vector<TextCursorStop> m_draftStops;
    float m_attachmentScroll = 0, m_attachmentX = 0, m_attachmentY = 0, m_attachmentWidth = 0;
    float m_draftX = 0, m_draftY = 0, m_draftWidth = 0, m_draftHeight = 0;
    std::uint64_t m_attachmentSerial = 0;
    std::vector<Hit> m_hits;
    float m_pointerX = 0, m_pointerY = 0;
    float m_guildScroll = 0, m_channelScroll = 0, m_messageScroll = 0, m_messageMaxScroll = 0, m_memberScroll = 0;
    bool m_revealGuild = false;
    float m_messageViewportTop = 122, m_messageViewportBottom = 600;
  };
} // namespace app
