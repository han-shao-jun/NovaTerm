#include "capabilities/CapabilityRegistry.h"
#include "capabilities/CapabilityDispatcher.h"
#include "capabilities/TypedCapabilityPort.h"
#include "capabilities/McpCapabilityAdapter.h"
#include <QtTest>
using namespace NovaTerm::Capabilities;
class CapabilityRuntimeTests : public QObject
{
    Q_OBJECT
private slots:
    void trustedAdmissionAndRevocation()
    {
        auto registry = CapabilityRegistry::terminalCapabilities();
        quint64 version = 1;
        bool allowed = true;
        CapabilityAuthorization auth{[&](const PrincipalId&, EntryKind, const ResourceBinding& resource,
                                         const Definition&) -> std::optional<AuthorizationSnapshot> {
            if (!allowed) return std::nullopt;
            return AuthorizationSnapshot{resource, version};
        }};
        CapabilityDispatcher dispatcher{registry, auth};
        const auto principal = auth.trust(PrincipalId{"client"}, EntryKind::Mcp);
        auto context = auth.context(principal, DirectoryBinding{}, version,
            OperationId{"one"}, Clock::now() + std::chrono::seconds(5));
        context.principal = PrincipalId{"forged"};
        auto denied = dispatcher.admit(CapabilityId{"session.list"}, context);
        QVERIFY(std::holds_alternative<Rejection>(denied));
        QCOMPARE(std::get<Rejection>(denied), Rejection::UntrustedPrincipal);
        context.principal = PrincipalId{"client"};
        auto admitted = dispatcher.admit(CapabilityId{"session.list"}, context);
        QVERIFY(std::holds_alternative<InvocationLease>(admitted));
        auto lease = std::move(std::get<InvocationLease>(admitted));
        QVERIFY(!lease.check());
        ++version;
        QCOMPARE(*lease.check(), Rejection::AuthorizationChanged);
        QVERIFY(!lease.finish());
        QCOMPARE(dispatcher.activeCount(), size_t(0));
        allowed = false;
        QVERIFY(std::holds_alternative<Rejection>(dispatcher.admit(CapabilityId{"session.list"}, context)));
    }
    void twoUnrelatedTypedPortsShareAdmission()
    {
        struct Read { int lines; };
        struct Analyze { QString text; };
        auto registry = CapabilityRegistry::terminalCapabilities();
        QVERIFY(registry.add({CapabilityId{"test.log.analyze"}, 1, ImplementationStatus::Implemented,
            ResourceKind::ImportedFile, Policy::ReadOutput, {}}));
        bool allowed = true;
        CapabilityAuthorization auth{[&](const PrincipalId&, EntryKind, const ResourceBinding& resource,
                                         const Definition&) -> std::optional<AuthorizationSnapshot> {
            return allowed ? std::optional<AuthorizationSnapshot>{{resource, 1}} : std::nullopt;
        }};
        CapabilityDispatcher dispatcher{registry, auth};
        int calls = 0;
        TypedCapabilityPort<Read, int> read{dispatcher, CapabilityId{"session.list"},
            [&](Read value, InvocationLease lease, auto done) { ++calls; QVERIFY(!lease.check()); done(value.lines); }};
        TypedCapabilityPort<Analyze, QString> analyze{dispatcher, CapabilityId{"test.log.analyze"},
            [&](Analyze value, InvocationLease, auto done) { ++calls; done(value.text.toUpper()); }};
        McpCapabilityAdapter mcp{auth, PrincipalId{"client"}};
        McpCapabilityAdapter second{auth, PrincipalId{"client"}};
        int readValue = 0;
        mcp.invoke(read, Read{4}, DirectoryBinding{}, 1, OperationId{"mcp"},
            Clock::now() + std::chrono::seconds(5), [&](auto result) { readValue = std::get<int>(result); });
        QCOMPARE(readValue, 4);
        second.invoke(read, Read{4}, DirectoryBinding{}, 1, OperationId{"second"},
            Clock::now() + std::chrono::seconds(5), [&](auto result) { readValue = std::get<int>(result); });
        QCOMPARE(readValue, 4);
        QString text;
        second.invoke(analyze, Analyze{"boot"}, ImportedFileBinding{ResourceId{"file"}, "hash", 4}, 1,
            OperationId{"analysis"}, Clock::now() + std::chrono::seconds(5),
            [&](auto result) { text = std::get<QString>(result); });
        QCOMPARE(text, QString("BOOT"));
        QCOMPARE(calls, 3);
        allowed = false;
        second.invoke(read, Read{4}, DirectoryBinding{}, 1, OperationId{"denied"},
            Clock::now() + std::chrono::seconds(5), [](auto result) { QVERIFY(std::holds_alternative<Rejection>(result)); });
        QCOMPARE(calls, 3);
        QCOMPARE(dispatcher.activeCount(), size_t(0));
    }
    void cancellationAndResourceSeparation()
    {
        const auto registry = CapabilityRegistry::terminalCapabilities();
        CapabilityAuthorization auth{[](const PrincipalId&, EntryKind, const ResourceBinding& resource,
                                        const Definition&) { return std::optional<AuthorizationSnapshot>{{resource, 1}}; }};
        CapabilityDispatcher dispatcher{registry, auth};
        const auto principal = auth.trust(PrincipalId{"client"}, EntryKind::Mcp);
        auto context = auth.context(principal, ImportedFileBinding{ResourceId{"file"}, "hash", 4}, 1,
            OperationId{"op"}, Clock::now() + std::chrono::seconds(5));
        QCOMPARE(std::get<Rejection>(dispatcher.admit(CapabilityId{"terminal.context.read"}, context)),
            Rejection::ResourceMismatch);
        context.resource = DirectoryBinding{};
        auto admitted = dispatcher.admit(CapabilityId{"session.list"}, context);
        auto lease = std::move(std::get<InvocationLease>(admitted));
        QVERIFY(!dispatcher.cancel(PrincipalId{"other"}, OperationId{"op"}));
        QVERIFY(dispatcher.cancel(PrincipalId{"client"}, OperationId{"op"}));
        QCOMPARE(*lease.check(), Rejection::Cancelled);
        // Cancellation is a delivery/admission fact, never remote termination evidence.
        QVERIFY(!lease.finish());
        QVERIFY(!lease.finish());
    }
    void revocationDeadlineAndBoundedAdmission()
    {
        static_assert(!std::is_copy_constructible_v<InvocationLease>);
        static_assert(std::is_nothrow_move_constructible_v<InvocationLease>);
        auto registry = CapabilityRegistry::terminalCapabilities();
        quint64 version = 1;
        auto now = Clock::now();
        CapabilityAuthorization auth{[&version](const PrincipalId&, EntryKind, const ResourceBinding& resource,
            const Definition&) { return std::optional<AuthorizationSnapshot>{{resource, version}}; }};
        CapabilityDispatcher dispatcher{registry, auth, [&now] { return now; }};
        McpCapabilityAdapter second{auth, PrincipalId{"client"}};
        auto context = second.context(DirectoryBinding{}, 1, OperationId{"queued"}, now + std::chrono::seconds(5));
        int calls = 0;
        Completion<int> delivery;
        std::optional<InvocationLease> held;
        TypedCapabilityPort<int, int> port{dispatcher, CapabilityId{"session.list"},
            [&calls, &delivery, &held](int, InvocationLease lease, auto done) {
                ++calls; held = std::move(lease); delivery = std::move(done);
            }};
        ++version;
        port.invoke(1, context, [](auto result) { QCOMPARE(std::get<Rejection>(result), Rejection::AuthorizationChanged); });
        QCOMPARE(calls, 0);
        context.accessVersion = version;
        int replies = 0;
        port.invoke(1, context, [&replies](auto result) {
            ++replies; QCOMPARE(std::get<Rejection>(result), Rejection::AuthorizationChanged);
        });
        ++version;
        delivery(42); delivery(42);
        QCOMPARE(replies, 1);
        QCOMPARE(calls, 1);
        held.reset(); delivery = {};
        std::vector<InvocationLease> leases;
        for (size_t i = 0; i < CapabilityDispatcher::MaxOperations; ++i) {
            context.operation = OperationId{QString::number(i)}; context.accessVersion = version;
            auto admitted = dispatcher.admit(CapabilityId{"session.list"}, context);
            QVERIFY(std::holds_alternative<InvocationLease>(admitted));
            leases.push_back(std::move(std::get<InvocationLease>(admitted)));
        }
        context.operation = OperationId{"overflow"};
        QCOMPARE(std::get<Rejection>(dispatcher.admit(CapabilityId{"session.list"}, context)), Rejection::Busy);
        now += std::chrono::seconds(5);
        QCOMPARE(*leases.front().check(), Rejection::DeadlineExceeded);
        leases.clear();
        QCOMPARE(dispatcher.activeCount(), size_t(0));
    }
    void catalogHasStableIds()
    {
        const auto registry = CapabilityRegistry::terminalCapabilities();
        QCOMPARE(registry.definitions().size(), size_t(5));
        const auto read = registry.find(CapabilityId{"terminal.context.read"});
        QVERIFY(read);
        QCOMPARE(read->mcpTool, QString("novaterm_read_context"));
        QCOMPARE(read->version, 1u);
        QVERIFY(!registry.find(CapabilityId{"novaterm_read_context"}));
        QVERIFY(registry.findByMcpTool("novaterm_list_sessions"));
    }
    void unimplementedToolsNotRegistered()
    {
        auto registry = CapabilityRegistry::terminalCapabilities();
        QVERIFY(!registry.find(CapabilityId{"log.analyze"}));
        auto definition = *registry.find(CapabilityId{"terminal.context.read"});
        QVERIFY(!registry.add(definition));
        definition.id = CapabilityId{"log.analyze"};
        definition.status = ImplementationStatus::Planned;
        QVERIFY(!registry.add(definition));
        QCOMPARE(registry.definitions().size(), size_t(5));
    }
};
QTEST_GUILESS_MAIN(CapabilityRuntimeTests)
#include "CapabilityRuntimeTests.moc"
