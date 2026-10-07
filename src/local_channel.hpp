#pragma once

#include "tls_identity.hpp"
#include "voice_session.hpp"
#include "chat_history.hpp"
#include "media_transport.hpp"
#include <QDateTime>
#include <QElapsedTimer>
#include <QCache>
#include <QBitArray>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QSslServer>
#include <QTimer>
#include <QThreadPool>
#include <QUdpSocket>
#include <QNetworkInterface>
#include <QVariantList>
#include <functional>
#include <memory>

// Hosts channels on one device endpoint, opens text sessions and joins one voice channel. All membership and
// presence messages use mutual TLS; names never authorize a peer.
class LocalChannel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool screenSharing READ screenSharing NOTIFY screenChanged)
    Q_PROPERTY(QVariantMap screenView READ screenView NOTIFY screenChanged)
    Q_PROPERTY(bool passwordProtected READ passwordProtected NOTIFY stateChanged)
    Q_PROPERTY(bool passwordBusy READ passwordBusy NOTIFY stateChanged)
    Q_PROPERTY(bool passwordRequired READ passwordRequired NOTIFY stateChanged)
    Q_PROPERTY(bool passwordSaved READ passwordSaved NOTIFY stateChanged)
    Q_PROPERTY(int passwordRetrySeconds READ passwordRetrySeconds NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool hosting READ hosting NOTIFY stateChanged)
    Q_PROPERTY(bool joined READ joined NOTIFY stateChanged)
    Q_PROPERTY(int receiveAudioBitrate READ receiveAudioBitrate NOTIFY stateChanged)
    Q_PROPERTY(bool requestsAllowed READ requestsAllowed NOTIFY stateChanged)
    Q_PROPERTY(bool directBusy READ directBusy NOTIFY stateChanged)
    Q_PROPERTY(int servicePort READ port NOTIFY stateChanged)
    Q_PROPERTY(int configuredPort READ configuredPort NOTIFY stateChanged)
    Q_PROPERTY(QString channelName READ channelName NOTIFY stateChanged)
    Q_PROPERTY(QString botName READ botName NOTIFY stateChanged)
    Q_PROPERTY(int messageLifetimeDays READ messageLifetimeDays NOTIFY stateChanged)
    Q_PROPERTY(int chatLifetimeDays READ chatLifetimeDays NOTIFY chatChanged)
    Q_PROPERTY(QVariantList controllers READ controllers NOTIFY controlChanged)
    Q_PROPERTY(QVariantList remoteOffers READ remoteOffers NOTIFY hostsChanged)
    Q_PROPERTY(QVariantList controlRequests READ controlRequests NOTIFY requestsChanged)
    Q_PROPERTY(QString controlTargetName READ controlTargetName NOTIFY controlChanged)
    Q_PROPERTY(QString controlStatus READ controlStatus NOTIFY controlChanged)
    Q_PROPERTY(bool controlConnected READ controlConnected NOTIFY controlChanged)
    Q_PROPERTY(bool remoteMode READ remoteMode NOTIFY remoteChanged)
    Q_PROPERTY(bool remoteAllowed READ remoteAllowed NOTIFY remoteChanged)
    Q_PROPERTY(QVariantMap remoteView READ remoteView NOTIFY remoteChanged)
    Q_PROPERTY(QVariantMap remoteLevels READ remoteLevels NOTIFY remoteLevelsChanged)
    Q_PROPERTY(QString ownId READ ownId NOTIFY stateChanged)
    Q_PROPERTY(QString channelId READ channelId NOTIFY stateChanged)
    Q_PROPERTY(QVariantList ownedChannels READ ownedChannels NOTIFY ownedChannelsChanged)
    Q_PROPERTY(QString chatHostId READ chatHostId NOTIFY chatChanged)
    Q_PROPERTY(QVariantList chatMembers READ chatMembers NOTIFY participantsChanged)
    Q_PROPERTY(QStringList chatOnlineIds READ chatOnlineIds NOTIFY participantsChanged)
    Q_PROPERTY(bool chatPresenceKnown READ chatPresenceKnown NOTIFY participantsChanged)
    Q_PROPERTY(QString joinedHostId READ joinedHostId NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QVariantList hosts READ hosts NOTIFY hostsChanged)
    Q_PROPERTY(QVariantList availableHosts READ availableHosts NOTIFY hostsChanged)
    Q_PROPERTY(QString discoveryError READ discoveryError NOTIFY hostsChanged)
    Q_PROPERTY(bool discoverySearching READ discoverySearching NOTIFY discoverySearchChanged)
    Q_PROPERTY(QVariantList savedChannels READ savedChannels NOTIFY hostsChanged)
    Q_PROPERTY(QVariantList participants READ participants NOTIFY participantsChanged)
    Q_PROPERTY(QVariantList audioSources READ audioSources NOTIFY participantsChanged)
    Q_PROPERTY(QVariantMap chatBot READ chatBot NOTIFY participantsChanged)
    Q_PROPERTY(QVariantList hostParticipants READ hostParticipants NOTIFY hostParticipantsChanged)
    Q_PROPERTY(QVariantList hostClients READ hostClients NOTIFY hostParticipantsChanged)
    Q_PROPERTY(QVariantList blockedClients READ blockedClients NOTIFY stateChanged)
    Q_PROPERTY(QVariantList requests READ requests NOTIFY requestsChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY chatChanged)
    Q_PROPERTY(bool chatReady READ chatReady NOTIFY chatChanged)
    Q_PROPERTY(bool chatPending READ chatPending NOTIFY chatChanged)
    Q_PROPERTY(bool historyLoading READ historyLoading NOTIFY chatChanged)
    Q_PROPERTY(bool hasOlderMessages READ hasOlderMessages NOTIFY chatChanged)
    Q_PROPERTY(bool hasNewerMessages READ hasNewerMessages NOTIFY chatChanged)
    Q_PROPERTY(QString chatError READ chatError NOTIFY chatChanged)
    Q_PROPERTY(int imageRevision READ imageRevision NOTIFY imagesChanged)
public:
    explicit LocalChannel(VoiceSession& session, QString storageFile,
                          std::optional<TlsIdentity> identity = std::nullopt,
                          std::function<qint64()> clock = QDateTime::currentMSecsSinceEpoch,
                          QObject* parent = nullptr);
    ~LocalChannel() override;
    [[nodiscard]] bool passwordProtected() const { return !security_.value("verifier").toString().isEmpty(); }
    [[nodiscard]] bool passwordBusy() const { return passwordBusy_; }
    [[nodiscard]] bool passwordRequired() const;
    [[nodiscard]] bool passwordSaved() const;
    [[nodiscard]] int passwordRetrySeconds() const;
    [[nodiscard]] static bool validHostPassword(const QString& password);
    Q_INVOKABLE bool setHostPassword(const QString& password);
    Q_INVOKABLE bool submitPassword(const QString& password, bool remember);
    Q_INVOKABLE bool forgetPassword();
    [[nodiscard]] bool ready() const { return identity_.has_value(); }
    [[nodiscard]] bool hosting() const { return hosting_; }
    [[nodiscard]] bool directBusy() const { return !direct_.isNull(); }
    [[nodiscard]] bool joined() const;
    [[nodiscard]] int receiveAudioBitrate() const;
    [[nodiscard]] bool requestsAllowed() const { return hostOnly_ || requestsAllowed_; }
    [[nodiscard]] QString ownId() const { return identity_ ? identity_->id() : QString{}; }
    [[nodiscard]] QString channelId() const { return channelId_.isEmpty() ? ownId() : channelId_; }
    [[nodiscard]] QVariantList ownedChannels() const;
    Q_INVOKABLE LocalChannel* ownChannel(const QString& id) const;
    Q_INVOKABLE QString addOwnedChannel(const QString& name);
    Q_INVOKABLE bool removeOwnedChannel(const QString& id);
    [[nodiscard]] QString radioStorageFile() const { return storageFile_ + ".radio.json"; }
    [[nodiscard]] QString joinedHostId() const { return joinedHostId_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] quint16 port() const { return service_ ? service_->port() : server_.serverPort(); }
    [[nodiscard]] int configuredPort() const { return service_ ? service_->configuredPort() : configuredPort_; }
    [[nodiscard]] QString channelName() const { return channelName_; }
    [[nodiscard]] QString botName() const { return botName_; }
    Q_INVOKABLE bool setBotName(const QString& name);
    [[nodiscard]] QVariantList audioSources() const;
    [[nodiscard]] QVariantMap chatBot() const;
    Q_INVOKABLE bool setChannelName(const QString& name);
    // Applies host-only settings atomically. Permission lists add entries;
    // existing bans survive approvals and can only be removed explicitly.
    bool configureHost(const QJsonObject& configuration);
    [[nodiscard]] QJsonObject hostConfiguration() const;
    [[nodiscard]] int messageLifetimeDays() const { return session_.supporterEnabled() ? messageLifetimeDays_ : std::min(messageLifetimeDays_, 30); }
    [[nodiscard]] int chatLifetimeDays() const;
    Q_INVOKABLE bool setConfiguredPort(int port);
    Q_INVOKABLE bool setMessageLifetimeDays(int days);
    [[nodiscard]] QVariantList hosts() const;
    [[nodiscard]] QVariantList availableHosts() const;
    [[nodiscard]] QString discoveryError() const { return discovery_.error; }
    [[nodiscard]] bool discoverySearching() const { return discovery_.searching; }
    [[nodiscard]] QVariantList savedChannels() const;
    [[nodiscard]] QVariantList participants() const;
    [[nodiscard]] QVariantList chatMembers() const;
    [[nodiscard]] QStringList chatOnlineIds() const;
    [[nodiscard]] bool chatPresenceKnown() const;
    [[nodiscard]] QString chatHostId() const { return chatHostId_; }
    [[nodiscard]] QVariantList hostParticipants() const;
    [[nodiscard]] QVariantList hostClients() const;
    [[nodiscard]] QVariantList blockedClients() const;
    [[nodiscard]] QVariantList requests() const;
    [[nodiscard]] QVariantList controllers() const;
    [[nodiscard]] QVariantList remoteOffers() const;
    [[nodiscard]] QVariantList controlRequests() const;
    [[nodiscard]] QVariantList messages() const;
    [[nodiscard]] bool chatReady() const;
    [[nodiscard]] bool chatPending() const;
    [[nodiscard]] bool historyLoading() const;
    [[nodiscard]] bool hasOlderMessages() const;
    [[nodiscard]] bool hasNewerMessages() const;
    Q_INVOKABLE bool loadOlderMessages();
    Q_INVOKABLE bool loadNewerMessages();
    Q_INVOKABLE bool refreshChat();
    [[nodiscard]] QString chatError() const;
    Q_INVOKABLE bool sendChat(const QString& text, const QByteArray& image = {});
    bool sendSystemMessage(const QString& text);
    // Reads one bounded page from this owned channel, without joining as a user.
    // query accepts cursor and direction (latest/older/newer). Invalid queries
    // throw invalid_argument; unavailable storage and I/O failures throw.
    [[nodiscard]] QJsonObject hostHistory(const QJsonObject& query = {});
    Q_INVOKABLE bool requestImage(const QString& hash);
    Q_INVOKABLE QString imageSource(const QString& hash) const;
    [[nodiscard]] int imageRevision() const { return imageRevision_; }
    [[nodiscard]] QString controlTargetName() const { return control_.value("target").toObject().value("name").toString(); }
    [[nodiscard]] QString controlStatus() const { return controlStatus_; }
    [[nodiscard]] bool controlConnected() const { return controlConnected_; }
    [[nodiscard]] bool remoteMode() const { return !hostOnly_ && control_.value("remoteMode").toBool(); }
    [[nodiscard]] bool remoteAllowed() const { return remoteAllowed_; }
    [[nodiscard]] QVariantMap remoteView() const { return remoteView_; }
    [[nodiscard]] QVariantMap remoteLevels() const { return remoteLevels_; }
    bool publishLevels(const QVariantMap& levels);
    Q_INVOKABLE bool initialize(bool hostOnly = false);
    Q_INVOKABLE bool startDiscovery();
    Q_INVOKABLE bool setDiscoverySearch(bool enabled);
    Q_INVOKABLE bool startHost();
    // The device endpoint also answers identity requests when no channel is open.
    static constexpr quint16 defaultPort = 48763;
    bool startService(const QHostAddress& address = QHostAddress::Any, quint16 port = defaultPort);
    bool listen(const QHostAddress& address, quint16 port = 0);
    Q_INVOKABLE bool stopHost();
    Q_INVOKABLE bool join(const QString& hostId, const QString& address, int port);
    Q_INVOKABLE bool joinAddress(const QString& endpoint);
    Q_INVOKABLE bool openAddress(const QString& endpoint);
    Q_INVOKABLE bool openChat(const QString& hostId, const QString& address, int port);
    Q_INVOKABLE bool openSavedChat(const QString& hostId);
    Q_INVOKABLE bool closeChat(const QString& hostId);
    Q_INVOKABLE bool joinSaved(const QString& hostId);
    Q_INVOKABLE bool setAutoJoin(const QString& hostId, bool enabled);
    Q_INVOKABLE bool removeChannel(const QString& hostId);
    Q_INVOKABLE bool approveAddress(const QString& endpoint);
    Q_INVOKABLE bool pairAddress(const QString& endpoint);
    Q_INVOKABLE bool decideControl(const QString& peerId, bool allow);
    Q_INVOKABLE bool setRemotePermission(const QString& peerId, bool allow);
    Q_INVOKABLE bool chooseRemoteOffer(const QString& hostId);
    Q_INVOKABLE bool setRemoteMode(bool enabled);
    Q_INVOKABLE bool remoteAction(const QString& action, const QVariantMap& data = {});
    Q_INVOKABLE bool clearControlTarget();
    Q_INVOKABLE bool leave();
    Q_INVOKABLE bool decide(const QString& peerId, bool allow);
    Q_INVOKABLE bool kick(const QString& peerId);
    Q_INVOKABLE bool setBlocked(const QString& peerId, bool blocked);
    Q_INVOKABLE bool setRequestsAllowed(bool allowed);
    // Payload entrypoints remain bounded and permission-checked; the host
    // attaches the authenticated sender rather than accepting a claimed one.
    bool sendAudio(const QByteArray& packet);
    bool setMusicState(const QString& name, const QString& state, bool active);
    bool sendMusic(const QByteArray& packet);
    [[nodiscard]] QString musicId() const;
    [[nodiscard]] bool screenSharing() const { return screenSharing_; }
    [[nodiscard]] QVariantMap screenView() const;
    Q_INVOKABLE QVariantMap screenInfo(const QString& hostId) const;
    // Only the owning capture pipeline may publish. Viewing inherits an existing
    // admitted text connection and uses a separate TLS stream on the same port.
    bool setScreenSharing(bool active);
    bool setScreenAudio(bool enabled);
    bool sendScreenAudio(const QByteArray& packet);
    [[nodiscard]] QSet<int> screenTiers() const;
    [[nodiscard]] bool screenNeedsKeyFrame(int tier) const;
    bool reportScreenEncodeTime(int milliseconds);
    bool sendScreenFrame(int tier, const QJsonObject& format, const QByteArray& packet, bool keyFrame);
    Q_INVOKABLE bool watchScreen(const QString& hostId, bool enabled, int minimumTier = 0, bool audio = false);
    bool acknowledgeScreen(const QString& hostId, qint64 serial, int decodeMilliseconds);

signals:
    void ownedChannelsChanged();
    void screenChanged();
    void screenFrameReceived(const QString& hostId, qint64 serial, const QJsonObject& format, const QByteArray& packet);
    void hostPasswordSaved(bool success);
    void stateChanged();
    void hostsChanged();
    void discoverySearchChanged();
    void participantsChanged();
    void hostParticipantsChanged();
    void requestsChanged();
    void controlChanged();
    void remoteChanged();
    void remoteLevelsChanged();
    void remoteChatSent(const QString& text);
    void remoteChatFailed(const QString& error);
    void audioReceived(const QString& peerId, const QByteArray& packet, int missing = 0);
    void chatChanged();
    void chatSent(const QString& text, const QString& hostId);
    void chatNotification(const QString& hostId, const QVariantMap& message);
    void imagesChanged();
    void channelEvent(const QString& kind);
private:
    LocalChannel(VoiceSession& session, QString storageFile, std::optional<TlsIdentity> identity,
                 std::function<qint64()> clock, QObject* parent, LocalChannel* service, QString channelId);
    LocalChannel* service_ = nullptr;
    QString channelId_;
    QStringList ownedIds_;
    QHash<QString, std::shared_ptr<LocalChannel>> owned_;
    bool syncOwnedChannels();
    QJsonArray channelDirectory() const;
    bool receiveDirectory(const QString& device, const QJsonObject& message, const QString& address,
                          quint16 port, bool direct, bool scanned);
    void acceptSocket(QSslSocket* socket);
    void dispatchMessage(QSslSocket* socket, const QJsonObject& message);
    struct Client final {
        QString id;
        QString deviceId;
        QString name;
        QString address;
        quint16 port = 0;
        QPointer<QSslSocket> socket;
        QByteArray buffer;
        QPointer<QSslSocket> screenSocket;
        QByteArray screenBuffer, screenPacket;
        QJsonObject screenFormat;
        qint64 screenSerial = 0, screenLastSerial = 0;
        QBitArray screenChunks;
        bool screenDatagram = false;
        std::shared_ptr<squad::MediaTransport> screenMedia;
        qsizetype screenSize = 0;
        bool screenAvailable = false, screenWanted = false;
        bool screenAudioAvailable = false, screenAudioWanted = false;
        int screenTier = -1, screenMinimumTier = 0;
        QString screenStatus;
        bool accepted = false;
        bool voice = false;
        bool adaptiveAudio = false;
        std::shared_ptr<squad::MediaTransport> media;
        int audioBitrate = 32;
        int messageLifetimeDays = 1;
        bool reconnectWanted = true;
        bool rosterSeen = false;
        QString access = QStringLiteral("connecting");
        QString status;
        QVariantList members;
        QStringList onlineIds;
        bool presenceKnown = false;
        qint64 reply = 0;
        qint64 probe = 0;
        qint64 reconnectAt = 0;
        QString password;
        qint64 passwordRetryAt = 0;
        bool passwordRequired = false;
        bool rememberPassword = false;
        bool passwordSent = false;
        bool passwordAutoRetry = false;
        QByteArray chatKey;
        QString chatEpoch;
        QJsonArray messages;
        qint64 lastChatSequence = 0;
        QJsonObject pending;
        QByteArray pendingImage;
        QString chatError;
        qint64 serverTime = 0;
        qint64 pendingUntil = 0;
        QElapsedTimer chatClock;
        QString historyRequest;
        QString historyDirection;
        qint64 historyCursor = 0;
        bool hasOlder = false;
        bool hasNewer = false;
        QSet<QString> capabilities;
        bool capabilitiesAdvertised = false;
    };
    bool screenBusy(const Client* receiver, const LocalChannel* sender) const;
    bool receivesScreenAudio(const Client& client) const;
    Client* chatClient() const;
    bool openConnection(const QString& id, const QString& address, int port, bool voice, bool automatic);
    QPair<QString, int> channelEndpoint(const QString& id) const;
    void selectChat(const QString& id);
    bool setClientStatus(Client& client, QString text, bool result = true);
    void closeClients();
    bool sendChatCommand(Client& client, const QJsonObject& payload);
    bool sendChatCommand(const QJsonObject& payload);
    bool submitPassword(Client& client, const QString& password, bool remember);
    struct Relay;
    struct Peer final {
        QString id;
        QString name;
        QString address;
        QString channel;
        quint16 servicePort = 0;
        QByteArray buffer;
        bool joined = false;
        bool closing = false;
        bool observer = false;
        bool adaptiveAudio = false;
        std::shared_ptr<squad::MediaTransport> media;
        bool remoteOffers = false;
        QSet<QString> capabilities;
        bool capabilitiesAdvertised = false;
        int audioTier = 0;
        int badAudioSamples = 0;
        int goodAudioSamples = 0;
        qint64 audioProbe = 0;
        qint64 audioProbeSent = -1;
        qint64 audioSampled = -1000;
        std::shared_ptr<Relay> relay;
        bool available = true;
        bool muted = true;
        bool deafened = false;
        bool sleeping = false;
        QString avatar = QStringLiteral("mossling");
        qint64 lastSpoke = 0;
        bool pending = false;
        bool controller = false;
        bool held = false;
        bool passwordChecking = false;
        QByteArray passwordVerified;
        qint64 revision = 0;
        qint64 lastActivity = 0;
        QJsonObject imageRequest;
        QByteArray imageUpload;
        bool imageProcessing = false;
        QByteArray remoteSnapshot;
        QString remoteSnapshotId;
        qint64 remoteSnapshotOffset = 0;
        bool remoteSnapshotWaiting = false;
        bool remoteSnapshotDirty = false;
        QJsonObject remoteImageRequest;
    };
    struct Viewer final {
        QString id;
        QByteArray buffer, packet;
        QJsonObject format;
        qsizetype offset = 0;
        qint64 serial = 0, sent = 0, goodSince = -1, resumeAt = 0;
        int tier = 1, sentTier = 1, minimumTier = 0, badSamples = 0;
        bool waiting = false, keyFrameNeeded = true;
        bool audioAllowed = false, audio = false, datagram = false;
        std::shared_ptr<squad::MediaTransport> media;
    };
    QHash<QSslSocket*, Viewer> viewers_;
    bool screenSharing_ = false, screenAudio_ = false;
    qint64 screenSequence_ = 0;
    int slowScreenEncodes_ = 0;
    QSet<QString> mediaParticipants() const;
    void acceptViewer(QSslSocket* socket, int minimumTier, bool audioAllowed, bool audio, bool udp);
    bool receivesScreenAudio(const Peer& peer) const;
    void readViewer(QSslSocket* socket);
    void finishScreen(QSslSocket* socket, qint64 serial, int decode, bool lost = false);
    void receiveScreenDatagram(Client& client, const QByteArray& bytes);
    void viewerMessage(QSslSocket* socket, const QJsonObject& message);
    void pumpScreen(QSslSocket* socket);
    void closeViewers(const QString& id = {});
    void connectScreen(Client& client);
    void closeScreen(Client& client);
    void receiveScreen(Client& client, const QJsonObject& message);
    QString musicName_;
    QString musicState_;
    bool musicActive_ = false;
    bool botSleeping_ = false;
    qint64 botLastActive_ = 0;
    std::shared_ptr<Relay> musicRelay_;
    struct Attempt final { int count = 0; qint64 next = 0; };
    struct Host final { QString name; QString address; quint16 port = 0; qint64 seen = 0; bool direct = false; bool scanned = false; QString deviceId; };
    bool setStatus(QString text, bool result = true);
    bool setControlStatus(QString text, bool result = true);
    bool persist(const QSet<QString>& approved, const QHash<QString, Attempt>& attempts, bool requestsAllowed,
                 const QHash<QString, QString>& pins, const QJsonObject& control, const QJsonObject& security,
                 const QJsonObject* channels = nullptr);
    bool beginJoin(const QString& hostId, const QString& address, int port, bool automatic);
    bool leaveChannel(bool pauseAutoJoin);
    void tryAutoJoin();
    void advanceAutoJoin(bool pause);
    bool rememberChannel(Client& client, const QString& name);
    enum class AddressAction { Join, Browse, Approve, Pair };
    bool accessAddress(const QString& endpoint, AddressAction action);
    void finishDirect();
    void endMembership(const Peer& peer);
    void finishPeer(QSslSocket* socket);
    void applyDecision(const QString& peerId, bool allow);
    bool disconnectMember(const QString& peerId, const QString& reason);
    bool persistControl(const QJsonObject& control);
    void updateControlIntent();
    bool startControl(const QString& id, const QString& address, quint16 port, const QString& name, bool pairing = true);
    bool receiveRemoteOffer(const QString& id, bool allowed, bool available = true);
    void publishRemoteAvailability();
    void connectControl();
    void closeControl();
    void blockControl(QString reason, bool revoked = false);
    void controlMessage(const QJsonObject& message);
    void sendControlState();
    void sendRemoteView(QSslSocket* socket = nullptr);
    void sendRemoteViewChunk(QSslSocket* socket);
    bool applyRemoteView(const QByteArray& bytes);
    bool receiveImage(const QJsonObject& payload);
    bool sendImageRequest(const QString& hash, qint64 offset);
    void sendRemoteImage(QSslSocket* socket);
    bool acceptControlState(QSslSocket* socket, const QJsonObject& message);
    bool handleRemoteAction(QSslSocket* socket, const QJsonObject& message);
    void acceptConnections();
    void acceptPeer(QSslSocket* socket);
    void admitPeer(QSslSocket* socket);
    void requirePassword(QSslSocket* socket, qint64 retryMs = 0, bool busy = false);
    void checkPassword(QSslSocket* socket, const QJsonObject& message);
    bool hashPassword(QByteArray password, QByteArray salt, std::function<void(QByteArray, QString)> completion);
    QByteArray passwordContext(const QString& host) const;
    bool savePassword(Client& client, const QString& password, bool remember);
    void readPeer(QSslSocket* socket);
    void hostMessage(QSslSocket* socket, const QJsonObject& message);
    void sampleAudioLink(QSslSocket* socket, int condition);
    void startMedia(QSslSocket* socket);
    void startMedia(Client& client);
    void hostAudio(QSslSocket* socket, const QByteArray& packet, int missing = 0);
    void relayAudio(QSslSocket* socket, const QByteArray& packet, bool screenAudio = false, int missing = 0);
    void clientMessage(Client& client, const QJsonObject& message);
    void connectClient(Client& client);
    void closeClient(Client& client);
    void sendPresence();
    void broadcastRoster();
    bool hostChat(QSslSocket* socket, const QJsonObject& message);
    bool clientChat(Client& client, const QJsonObject& message);
    bool requestHistory(Client& client, const QString& direction);
    void trimHistory(Client& client, bool older);
    void rotateChatKey();
    void sendChatKey(QSslSocket* socket);
    bool writeChat(QSslSocket* socket, const QJsonObject& payload);
    void retryChat(Client& client);
    void publishChat(const QJsonObject& record);
    void publishMembership(const QString& kind, const QString& member, const QString& name);
    bool publishSystem(const QString& text, const QJsonObject& event);
    [[nodiscard]] QVariantMap hostBot() const;
    bool visibleImage(const QString& hash) const;
    void downloadImage();
    bool hostImage(QSslSocket* socket, const QJsonObject& payload, qint64 now);
    static constexpr qsizetype maximumRemoteView = 2 * 1024 * 1024;
    static constexpr int maximumWindowMessages = 160;
    static constexpr qsizetype maximumWindowBytes = 512 * 1024;
    static constexpr qsizetype remoteChunkSize = 16384;
    static constexpr qsizetype maximumFrame = 65536;
    static bool parseCapabilities(const QJsonObject& message, QSet<QString>* negotiated = nullptr);
    static bool validMessageType(const QString& type);
    static bool validEndpointHost(const QString& value);
    static void connectEndpoint(QSslSocket* socket, const QString& host, quint16 port, QAbstractSocket::NetworkLayerProtocol protocol);
    static bool addressConnectionFailure(QAbstractSocket::SocketError error, const QSslSocket* socket);
    static QJsonArray capabilityAdvertisement();
    static QString canonicalEndpointHost(const QString& value);
    static QString endpointKey(const QString& host, quint16 port);
    static bool validPresenceDetails(const QJsonObject& message);
    static bool displayName(const QJsonValue& value);
    static bool acceptableCertificateErrors(const QList<QSslError>& errors);
    static bool writeMessage(QSslSocket* socket, const QJsonObject& message);
    static bool readMessages(QSslSocket* socket, QByteArray& buffer,
                             const std::function<void(const QJsonObject&)>& receive);
    static bool unknownMessageType(const QString& type);
    static bool validId(const QString& value);
    static bool validMembers(const QJsonValue& value);
    static bool localAddress(const QHostAddress& address);
    VoiceSession& session_;
    QString storageFile_;
    QString channelName_ = "My channel";
    QString botName_ = "System";
    int configuredPort_ = defaultPort;
    int messageLifetimeDays_ = 1;
    std::optional<TlsIdentity> identity_;
    std::function<qint64()> clock_;
    QSslServer server_;
    // Owns discovery sockets, interface membership and bounded scan work.
    // Admission and channel-directory validation remain with the channel.
    class Discovery final : public QObject {
    public:
        explicit Discovery(LocalChannel& owner);
        ~Discovery() override;
        bool start();
        bool search(bool enabled);
        void clearSearch();
        void announce();
        QHash<QString, Host> hosts;
        QString error;
        bool searching = false;
    private:
        struct Target final { QString address; quint16 port = 0; };
        struct Probe final { Target target; QByteArray buffer; };
        QList<Target> targets() const;
        void pump();
        void finishProbe(QSslSocket* socket);
        void receive();
        LocalChannel& owner_;
        QUdpSocket socket_;
        QList<QNetworkInterface> interfaces_;
        QTimer heartbeat_;
        QList<Target> queue_;
        QHash<QSslSocket*, Probe> probes_;
    } discovery_{*this};
    QTimer controlTimer_;
    QTimer maintenanceTimer_;
    QTimer autoJoinTimer_;
    std::unique_ptr<ChatHistory> history_;
    QByteArray hostChatKey_;
    QString hostChatEpoch_;
    QString chatError_;
    QHash<QString, QPair<qint64, int>> chatWindows_;
    QStringList imageQueue_;
    QString downloadingImage_;
    QByteArray downloadedImage_;
    QCache<QString, QByteArray> imageCache_{32 * 1024 * 1024};
    int imageRevision_ = 0;
    QElapsedTimer controlClock_;
    qint64 controlReply_ = 0;
    qint64 controlAckRevision_ = -1;
    QHash<QSslSocket*, Peer> peers_;
    QJsonObject pendingDepartureReasons_;
    QSet<QString> approved_;
    QHash<QString, Attempt> attempts_;
    QHash<QString, QString> endpointPins_;
    QJsonObject security_{{"verifier", ""}, {"saved", QJsonObject{}}};
    QThreadPool passwordWorkers_;
    int passwordChecks_ = 0;
    bool passwordBusy_ = false;
    QJsonObject control_{{"controllers", QJsonObject{}}, {"target", QJsonObject{}}, {"remoteMode", false}};
    QSet<QString> unavailableControlTargets_;
    QJsonObject savedChannels_;
    QSet<QString> pausedAutoJoin_;
    QSet<QString> triedAutoJoin_;
    bool automaticAttempt_ = false;
    QHash<QString, QPair<qint64, int>> requestWindows_;
    QPointer<QSslSocket> direct_;
    QPointer<QSslSocket> controller_;
    QByteArray controlBuffer_;
    QByteArray directBuffer_;
    QString joinedHostId_;
    QString chatHostId_;
    QHash<QString, std::shared_ptr<Client>> clients_;
    QString status_ = QStringLiteral("Network not started yet.");
    QString controlStatus_ = QStringLiteral("No remote control configured.");
    bool controlConnected_ = false;
    bool remoteAllowed_ = false;
    QVariantMap remoteView_;
    QVariantMap remoteLevels_;
    QByteArray remoteSnapshotBuffer_;
    QString remoteSnapshotId_;
    qint64 remoteSnapshotExpected_ = -1;
    QPointer<QSslSocket> remoteChatSocket_;
    QString remotePendingChat_;
    QString remotePendingHost_;
    bool controlPending_ = false;
    bool controlStopped_ = false;
    bool requestsAllowed_ = true;
    bool hosting_ = false;
    bool initializing_ = false;
    bool hostOnly_ = false;
    bool rosterPending_ = false;
    bool remoteViewPending_ = false;
};
