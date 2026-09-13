/**
 * @file   CredentialStore.cpp
 * @brief  凭据存储实现：平台密钥链与内存回退。
 *
 * Windows 通过 CredWriteW/CredReadW/CredDeleteW 对接 Credential Manager，凭据
 * 目标名统一为 "NovaTerm/<reference>"；Linux/BSD 通过 QtDBus 对接 freedesktop
 * Secret Service（`org.freedesktop.secrets`），凭据交给密钥环加密保管；两者都
 * 不可用时回退 MemoryCredentialStore，析构时主动清零内存。
 */
#include "CredentialStore.h"

#include <QDebug>
#include <QEventLoop>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#endif

namespace {

#ifdef Q_OS_WIN
QString credentialTarget(const QString& reference)
{
    return QStringLiteral("NovaTerm/%1").arg(reference);
}

class WindowsCredentialStore final : public CredentialStore
{
public:
    bool put(const QString& reference, const QByteArray& secret) override
    {
        if (reference.trimmed().isEmpty() || secret.isEmpty())
            return false;

        const QString target = credentialTarget(reference);
        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = const_cast<wchar_t*>(
            reinterpret_cast<const wchar_t*>(target.utf16()));
        credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(
            const_cast<char*>(secret.constData()));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        return CredWriteW(&credential, 0) != FALSE;
    }

    [[nodiscard]] std::optional<QByteArray>
    get(const QString& reference) const override
    {
        if (reference.trimmed().isEmpty())
            return std::nullopt;

        const QString target = credentialTarget(reference);
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(reinterpret_cast<const wchar_t*>(target.utf16()),
                       CRED_TYPE_GENERIC, 0, &credential)) {
            return std::nullopt;
        }

        const QByteArray secret(
            reinterpret_cast<const char*>(credential->CredentialBlob),
            static_cast<qsizetype>(credential->CredentialBlobSize));
        CredFree(credential);
        return secret;
    }

    bool remove(const QString& reference) override
    {
        if (reference.trimmed().isEmpty())
            return false;
        const QString target = credentialTarget(reference);
        if (CredDeleteW(reinterpret_cast<const wchar_t*>(target.utf16()),
                        CRED_TYPE_GENERIC, 0)) {
            return true;
        }
        return GetLastError() == ERROR_NOT_FOUND;
    }

    [[nodiscard]] bool isPersistent() const override { return true; }
};
#endif // Q_OS_WIN

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)

/// 会话总线上的服务名（gnome-keyring-daemon、KWallet 都注册它）。
const QString SecretsService = QStringLiteral("org.freedesktop.secrets");
const QString ServicePath = QStringLiteral("/org/freedesktop/secrets");
const QString ServiceInterface = QStringLiteral("org.freedesktop.Secret.Service");
const QString CollectionInterface = QStringLiteral("org.freedesktop.Secret.Collection");
const QString ItemInterface = QStringLiteral("org.freedesktop.Secret.Item");
const QString PromptInterface = QStringLiteral("org.freedesktop.Secret.Prompt");
/// Secret Service 用 "/" 表示"没有提示/没有集合"。
const QString NoObjectPath = QStringLiteral("/");
/// 条目属性：NovaTerm 自己检索用，密钥环里也能一眼认出是谁写的。
const QString AttributeService = QStringLiteral("service");
const QString AttributeServiceValue = QStringLiteral("NovaTerm");
const QString AttributeReference = QStringLiteral("novaterm-ref");
const QString LabelProperty = QStringLiteral("org.freedesktop.Secret.Item.Label");
const QString AttributesProperty = QStringLiteral("org.freedesktop.Secret.Item.Attributes");

/// 普通调用超时：密钥环应答是本地 IPC，正常在毫秒级。
constexpr int CallTimeoutMs = 10000;
/// 解锁提示超时：等的是用户敲密钥环口令，给足时间。
constexpr int PromptTimeoutMs = 120000;

/**
 * @brief Secret Service 的 Secret 结构 `(oayays)`。
 *
 * 依次是会话路径、算法参数、凭据字节、内容类型。QtDBus 需要显式注册签名与
 * 流操作才能双向编解码，见 registerSecretValueType()。
 */
struct SecretValue
{
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

QDBusArgument& operator<<(QDBusArgument& argument, const SecretValue& secret)
{
    argument.beginStructure();
    argument << secret.session << secret.parameters << secret.value
             << secret.contentType;
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, SecretValue& secret)
{
    argument.beginStructure();
    argument >> secret.session >> secret.parameters >> secret.value
             >> secret.contentType;
    argument.endStructure();
    return argument;
}

/// 注册 `(oayays)`：只注册流操作还不够，签名也要给，否则调用参数编不出来。
/// 顺便注册 `a{ss}`：Secret Service 的属性参数是 `QMap<QString,QString>`，
/// 不注册会以 "Unregistered type" 直接发给总线失败。
void registerSecretValueType()
{
    static const bool registered = [] {
        const QMetaType type = qDBusRegisterMetaType<SecretValue>();
        QDBusMetaType::registerCustomType(type, QByteArrayLiteral("(oayays)"));
        qDBusRegisterMetaType<QMap<QString, QString>>();
        return true;
    }();
    Q_UNUSED(registered)
}

/// 把 `ao` 解成对象路径列表。Qt 对数组**参数**不做内建映射：拿到的是
/// QDBusArgument，必须自己按数组流出来（只有单值 `o` 才直接给 QDBusObjectPath）。
/// 同时兼容将来 Qt 直接给出 QList<QDBusObjectPath> 的情况。
QList<QDBusObjectPath> objectPathList(const QVariant& value)
{
    if (value.metaType() == QMetaType::fromType<QList<QDBusObjectPath>>())
        return value.value<QList<QDBusObjectPath>>();
    QList<QDBusObjectPath> paths;
    if (value.metaType() != QMetaType::fromType<QDBusArgument>())
        return paths;

    const QDBusArgument argument = value.value<QDBusArgument>();
    argument.beginArray();
    while (!argument.atEnd()) {
        QDBusObjectPath path;
        argument >> path;
        paths.append(path);
    }
    argument.endArray();
    return paths;
}

/// 把 `(oayays)` 解成 SecretValue：注册过签名时 Qt 直接给结构体，否则给
/// QDBusArgument（与 `ao` 同理）。
std::optional<SecretValue> decodeSecret(const QVariant& value)
{
    if (value.metaType() == QMetaType::fromType<SecretValue>())
        return value.value<SecretValue>();
    if (value.metaType() != QMetaType::fromType<QDBusArgument>())
        return std::nullopt;

    const QDBusArgument argument = value.value<QDBusArgument>();
    SecretValue secret;
    argument >> secret;
    return secret;
}

#endif // Q_OS_UNIX && !Q_OS_MACOS

} // namespace

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
/**
 * @brief `org.freedesktop.Secret.Prompt.Completed` 的槽载体。
 *
 * Prompt 对象只能用信号回报结果，而 QDBusConnection::connect() 需要 QObject
 * 槽（Qt 6.8 没有可调用对象重载），故单独定义一个最小 QObject。
 */
class SecretPromptWatcher : public QObject
{
    Q_OBJECT
public:
    /// 用户是否取消了提示（取消即操作未执行）。
    bool dismissed{true};
    /// 是否收到了 Completed 信号（超时则为 false）。
    bool finished{false};

public slots:
    void onCompleted(bool wasDismissed, const QDBusVariant&)
    {
        dismissed = wasDismissed;
        finished = true;
        emit completed();
    }

signals:
    void completed();
};

// 注意：Impl 是 SecretServiceCredentialStore 的嵌套类，定义必须落在全局作用域
// ——放进匿名命名空间会变成另一个类，成员函数与构造处看到的类型就对不上了。
class SecretServiceCredentialStore::Impl
{
public:
    Impl() { registerSecretValueType(); }

    bool put(const QString& reference, const QByteArray& secret) const;
    [[nodiscard]] std::optional<QByteArray> get(const QString& reference) const;
    bool remove(const QString& reference) const;

    [[nodiscard]] static bool isAvailable();

private:
    [[nodiscard]] static QDBusMessage call(const QString& path,
                                           const QString& interface,
                                           const QString& method,
                                           const QVariantList& arguments,
                                           int timeoutMs = CallTimeoutMs);
    /// 打开一个 `plain` 会话，返回会话路径；失败返回空串。
    [[nodiscard]] static QString openSession();
    /// 默认集合（alias `default`）路径；没有默认集合返回空串。
    [[nodiscard]] static QString defaultCollection();
    /// 解锁集合，必要时处理系统解锁提示。
    [[nodiscard]] static bool unlock(const QString& collection);
    /// 调用 Prompt() 并等 Completed 信号；返回 true 表示用户确认。
    [[nodiscard]] static bool runPrompt(const QString& promptPath);
    [[nodiscard]] static QList<QDBusObjectPath> searchItems(const QString& reference);
    [[nodiscard]] static bool deleteItem(const QString& itemPath);
    [[nodiscard]] static QMap<QString, QString> attributesFor(const QString& reference);
};

QDBusMessage SecretServiceCredentialStore::Impl::call(const QString& path,
                                                      const QString& interface,
                                                      const QString& method,
                                                      const QVariantList& arguments,
                                                      int timeoutMs)
{
    QDBusMessage message =
        QDBusMessage::createMethodCall(SecretsService, path, interface, method);
    message.setArguments(arguments);
    return QDBusConnection::sessionBus().call(message, QDBus::Block, timeoutMs);
}

QString SecretServiceCredentialStore::Impl::openSession()
{
    // "plain" 算法不加密载荷：会话总线本身只对同一用户开放，密钥环的落盘加密
    // 与它无关，gnome-keyring/KWallet 都支持该算法且不会弹提示。
    const QDBusMessage reply = call(
        ServicePath, ServiceInterface, QStringLiteral("OpenSession"),
        {QStringLiteral("plain"), QVariant::fromValue(QDBusVariant(QString()))});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        qWarning() << "Secret Service OpenSession failed:" << reply.errorMessage();
        return {};
    }
    return reply.arguments().at(1).value<QDBusObjectPath>().path();
}

QString SecretServiceCredentialStore::Impl::defaultCollection()
{
    const QDBusMessage reply =
        call(ServicePath, ServiceInterface, QStringLiteral("ReadAlias"),
             {QStringLiteral("default")});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        qWarning() << "Secret Service ReadAlias(default) failed:"
                   << reply.errorMessage();
        return {};
    }
    const QString path = reply.arguments().first().value<QDBusObjectPath>().path();
    return path == NoObjectPath ? QString() : path;
}

bool SecretServiceCredentialStore::Impl::runPrompt(const QString& promptPath)
{
    if (promptPath.isEmpty() || promptPath == NoObjectPath)
        return true;

    QDBusConnection bus = QDBusConnection::sessionBus();
    SecretPromptWatcher watcher;
    if (!bus.connect(SecretsService, promptPath, PromptInterface,
                     QStringLiteral("Completed"), &watcher,
                     SLOT(onCompleted(bool, QDBusVariant)))) {
        qWarning() << "Failed to subscribe to the Secret Service prompt result";
        return false;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&watcher, &SecretPromptWatcher::completed, &loop,
                     &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    // Prompt() 立即返回，结果通过 Completed 信号送达；空 window id 交给密钥环
    // 自己的提示器（gnome-keyring 的 PrivatePrompter）。
    const QDBusMessage promptReply =
        call(promptPath, PromptInterface, QStringLiteral("Prompt"), {QString()}, 5000);
    if (promptReply.type() == QDBusMessage::ErrorMessage) {
        qWarning() << "Secret Service prompt call failed:" << promptReply.errorMessage();
        return false;
    }
    timeout.start(PromptTimeoutMs);
    if (!watcher.finished)
        loop.exec();
    bus.disconnect(SecretsService, promptPath, PromptInterface,
                   QStringLiteral("Completed"), &watcher,
                   SLOT(onCompleted(bool, QDBusVariant)));

    if (!watcher.finished) {
        qWarning() << "Timed out waiting for the keyring unlock prompt";
        return false;
    }
    return !watcher.dismissed;
}

bool SecretServiceCredentialStore::Impl::unlock(const QString& collection)
{
    const QDBusMessage reply = call(
        ServicePath, ServiceInterface, QStringLiteral("Unlock"),
        {QVariant::fromValue(QList<QDBusObjectPath>{QDBusObjectPath(collection)})});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        qWarning() << "Secret Service Unlock failed:" << reply.errorMessage();
        return false;
    }
    const QString prompt = reply.arguments().at(1).value<QDBusObjectPath>().path();
    return runPrompt(prompt);
}

QMap<QString, QString>
SecretServiceCredentialStore::Impl::attributesFor(const QString& reference)
{
    // 参数类型是 a{ss}：必须用 QMap<QString,QString>，QVariantMap 会编成 a{sv}。
    QMap<QString, QString> attributes;
    attributes.insert(AttributeService, AttributeServiceValue);
    attributes.insert(AttributeReference, reference);
    return attributes;
}

QList<QDBusObjectPath>
SecretServiceCredentialStore::Impl::searchItems(const QString& reference)
{
    QList<QDBusObjectPath> items;
    const QDBusMessage reply =
        call(ServicePath, ServiceInterface, QStringLiteral("SearchItems"),
             {QVariant::fromValue(attributesFor(reference))});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        qWarning() << "Secret Service SearchItems failed:" << reply.errorMessage();
        return items;
    }
    // 前一个是已解锁条目，后一个是锁定条目；两者都要算上。
    items = objectPathList(reply.arguments().at(0));
    items += objectPathList(reply.arguments().at(1));
    return items;
}

bool SecretServiceCredentialStore::Impl::deleteItem(const QString& itemPath)
{
    const QDBusMessage reply =
        call(itemPath, ItemInterface, QStringLiteral("Delete"), {});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        qWarning() << "Secret Service item Delete failed:" << reply.errorMessage();
        return false;
    }
    return runPrompt(reply.arguments().first().value<QDBusObjectPath>().path());
}

std::optional<QByteArray>
SecretServiceCredentialStore::Impl::get(const QString& reference) const
{
    if (reference.trimmed().isEmpty())
        return std::nullopt;

    // 每次操作新建会话：会话只在密钥环进程内有效，缓存它反而要处理密钥环重启
    // 后的失效，而读写凭据不在热路径上。
    const QString session = openSession();
    if (session.isEmpty())
        return std::nullopt;

    const QList<QDBusObjectPath> items = searchItems(reference);
    if (items.isEmpty())
        return std::nullopt;

    const QString collection = defaultCollection();
    if (!collection.isEmpty() && !unlock(collection))
        return std::nullopt;

    const QDBusObjectPath sessionPath(session);
    for (const QDBusObjectPath& item : items) {
        const QDBusMessage reply =
            call(item.path(), ItemInterface, QStringLiteral("GetSecret"),
                 {QVariant::fromValue(sessionPath)});
        if (reply.type() != QDBusMessage::ReplyMessage
            || reply.arguments().isEmpty()) {
            qWarning() << "Secret Service GetSecret failed:" << reply.errorMessage();
            continue;
        }
        const std::optional<SecretValue> secret =
            decodeSecret(reply.arguments().first());
        if (secret.has_value() && !secret->value.isEmpty())
            return secret->value;
    }
    return std::nullopt;
}

bool SecretServiceCredentialStore::Impl::put(const QString& reference,
                                             const QByteArray& secret) const
{
    if (reference.trimmed().isEmpty() || secret.isEmpty())
        return false;

    const QString session = openSession();
    if (session.isEmpty())
        return false;
    const QString collection = defaultCollection();
    if (collection.isEmpty()) {
        qWarning() << "No default Secret Service collection; cannot store credentials";
        return false;
    }
    if (!unlock(collection))
        return false;

    // 先清掉同名旧条目：CreateItem 的 replace 语义在不同实现上不一致，旧条目
    // 留着会让 get() 读到过期密码。删不掉不算致命，CreateItem 仍可能覆盖。
    const QList<QDBusObjectPath> existing = searchItems(reference);
    for (const QDBusObjectPath& item : existing) {
        if (!deleteItem(item.path())) {
            qWarning() << "Failed to delete the previous Secret Service item for"
                       << reference;
        }
    }

    QVariantMap properties;
    properties.insert(LabelProperty,
                      QStringLiteral("NovaTerm/%1").arg(reference));
    properties.insert(AttributesProperty,
                      QVariant::fromValue(attributesFor(reference)));
    const SecretValue value{QDBusObjectPath(session), QByteArray(), secret,
                            QStringLiteral("text/plain")};

    const QDBusMessage reply =
        call(collection, CollectionInterface, QStringLiteral("CreateItem"),
             {properties, QVariant::fromValue(value), true});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        qWarning() << "Secret Service CreateItem failed:" << reply.errorMessage();
        return false;
    }
    const QString item = reply.arguments().at(0).value<QDBusObjectPath>().path();
    const QString prompt = reply.arguments().at(1).value<QDBusObjectPath>().path();
    if (!runPrompt(prompt))
        return false;
    return item != NoObjectPath;
}

bool SecretServiceCredentialStore::Impl::remove(const QString& reference) const
{
    if (reference.trimmed().isEmpty())
        return false;

    const QList<QDBusObjectPath> items = searchItems(reference);
    if (items.isEmpty())
        return false;

    bool removed = false;
    for (const QDBusObjectPath& item : items)
        removed = deleteItem(item.path()) || removed;
    return removed;
}

bool SecretServiceCredentialStore::Impl::isAvailable()
{
    if (!QDBusConnection::sessionBus().isConnected())
        return false;
    // ReadAlias 会按需启动密钥环服务；没有服务时总线直接回错误，不会卡满超时。
    return !defaultCollection().isEmpty();
}

SecretServiceCredentialStore::SecretServiceCredentialStore()
    : _impl(std::make_unique<Impl>())
{
}

SecretServiceCredentialStore::~SecretServiceCredentialStore() = default;

bool SecretServiceCredentialStore::put(const QString& reference,
                                       const QByteArray& secret)
{
    return _impl->put(reference, secret);
}

std::optional<QByteArray>
SecretServiceCredentialStore::get(const QString& reference) const
{
    return _impl->get(reference);
}

bool SecretServiceCredentialStore::remove(const QString& reference)
{
    return _impl->remove(reference);
}

bool SecretServiceCredentialStore::isServiceAvailable()
{
    return Impl::isAvailable();
}

#endif // Q_OS_UNIX && !Q_OS_MACOS

MemoryCredentialStore::~MemoryCredentialStore()
{
    for (QByteArray& secret : _secrets)
        secret.fill('\0');
}

bool MemoryCredentialStore::put(const QString& reference,
                                const QByteArray& secret)
{
    if (reference.trimmed().isEmpty() || secret.isEmpty())
        return false;
    auto it = _secrets.find(reference);
    if (it != _secrets.end())
        it->fill('\0');
    _secrets.insert(reference, secret);
    return true;
}

std::optional<QByteArray>
MemoryCredentialStore::get(const QString& reference) const
{
    const auto it = _secrets.constFind(reference);
    return it == _secrets.cend() ? std::nullopt
                                 : std::optional<QByteArray>(*it);
}

bool MemoryCredentialStore::remove(const QString& reference)
{
    auto it = _secrets.find(reference);
    if (it == _secrets.end())
        return false;
    it->fill('\0');
    _secrets.erase(it);
    return true;
}

std::unique_ptr<CredentialStore> createCredentialStore()
{
#if defined(Q_OS_WIN)
    return std::make_unique<WindowsCredentialStore>();
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    if (SecretServiceCredentialStore::isServiceAvailable())
        return std::make_unique<SecretServiceCredentialStore>();
    // 没有密钥环时退回内存：保存的密码活不过本次进程，历史会话重连会取不到
    // 凭据（界面会提示重新输入）。这里明确告警，避免又变成"静默失效"。
    qWarning() << "freedesktop Secret Service is unavailable (no session bus or no"
               << "default keyring); credentials stay in memory and saved sessions"
               << "cannot reconnect after a restart";
    return std::make_unique<MemoryCredentialStore>();
#else
    qWarning() << "No platform credential backend on this platform; credentials"
               << "stay in memory and saved sessions cannot reconnect after a restart";
    return std::make_unique<MemoryCredentialStore>();
#endif
}

// SecretPromptWatcher 只在 Secret Service 后端存在时才编译，AUTOMOC 也只在
// 该平台生成这个 .moc，因此 include 要放进同一个条件里。
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
#include "CredentialStore.moc"
#endif
