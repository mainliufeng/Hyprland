#include <managers/SeatClientRegistry.hpp>
#include <helpers/WaylandSocket.hpp>
#include <gtest/gtest.h>
#include <hyprutils/os/FileDescriptor.hpp>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <system_error>
#include <stdexcept>
#include <array>
#include <memory>
#include <filesystem>
#include <format>
#include <atomic>
#include <algorithm>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// This registry only uses desktop pointer identity. Real Wayland clients exercise
// the destroy listener without constructing a compositor, renderer or desktop.
class CSeatClientRegistryTest : public ::testing::Test {
  protected:
    void                                                       SetUp() override;
    void                                                       TearDown() override;
    wl_client*                                                 newClient(wl_display* display = nullptr);
    CSeatDesktop*                                              seat(size_t index);

    std::unique_ptr<wl_display, decltype(&wl_display_destroy)> m_display{nullptr, wl_display_destroy};
    std::vector<Hyprutils::OS::CFileDescriptor>                m_peers;
    std::array<std::byte, 2>                                   m_seatIdentities = {};
    CSeatClientRegistry                                        m_registry;
};

void CSeatClientRegistryTest::SetUp() {
    m_display.reset(wl_display_create());
    ASSERT_NE(m_display, nullptr);
}

void CSeatClientRegistryTest::TearDown() {
    wl_display_destroy_clients(m_display.get());
}

wl_client* CSeatClientRegistryTest::newClient(wl_display* display) {
    std::array<int, 2> sockets = {
        -1,
        -1,
    };
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) < 0)
        return nullptr;
    Hyprutils::OS::CFileDescriptor server{sockets[0]};
    m_peers.emplace_back(sockets[1]);
    auto client = wl_client_create(display ? display : m_display.get(), server.get());
    if (client)
        server.take();
    return client;
}

CSeatDesktop* CSeatClientRegistryTest::seat(size_t index) {
    return rc<CSeatDesktop*>(&m_seatIdentities.at(index));
}

TEST_F(CSeatClientRegistryTest, UnknownAndNullClientsHaveNoPreferredSeat) {
    const auto client = newClient();
    ASSERT_NE(client, nullptr);
    EXPECT_EQ(m_registry.seatFor(client), nullptr);
    EXPECT_EQ(m_registry.seatFor(nullptr), nullptr);
    EXPECT_FALSE(m_registry.hasClients(seat(0)));
    EXPECT_FALSE(m_registry.hasClients(nullptr));
    m_registry.add(nullptr, seat(0));
    m_registry.add(client, nullptr);
    EXPECT_EQ(m_registry.seatFor(client), nullptr);
}

TEST_F(CSeatClientRegistryTest, ConnectionsKeepIndependentPreferredSeats) {
    const auto first  = newClient();
    const auto second = newClient();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    m_registry.add(first, seat(0));
    m_registry.add(second, seat(1));
    EXPECT_EQ(m_registry.seatFor(first), seat(0));
    EXPECT_EQ(m_registry.seatFor(second), seat(1));
    EXPECT_TRUE(m_registry.hasClients(seat(0)));
    EXPECT_TRUE(m_registry.hasClients(seat(1)));
}

TEST_F(CSeatClientRegistryTest, ExistingConnectionsCannotMoveToReplacementSeats) {
    const auto client = newClient();
    ASSERT_NE(client, nullptr);
    m_registry.add(client, seat(0));
    m_registry.add(client, seat(0));
    m_registry.add(client, seat(1));
    EXPECT_EQ(m_registry.seatFor(client), seat(0));
    EXPECT_TRUE(m_registry.hasClients(seat(0)));
    EXPECT_FALSE(m_registry.hasClients(seat(1)));
    wl_client_destroy(client);
    EXPECT_FALSE(m_registry.hasClients(seat(0)));
}

TEST_F(CSeatClientRegistryTest, ClientDestroyRevokesOnlyItsOwnMapping) {
    const auto first  = newClient();
    const auto second = newClient();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    m_registry.add(first, seat(0));
    m_registry.add(second, seat(1));
    wl_client_destroy(first);
    EXPECT_EQ(m_registry.seatFor(first), nullptr);
    EXPECT_FALSE(m_registry.hasClients(seat(0)));
    EXPECT_EQ(m_registry.seatFor(second), seat(1));
    EXPECT_TRUE(m_registry.hasClients(seat(1)));
}

TEST_F(CSeatClientRegistryTest, SeatRemainsRegisteredUntilItsLastClientDisconnects) {
    const auto first  = newClient();
    const auto second = newClient();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    m_registry.add(first, seat(0));
    m_registry.add(second, seat(0));
    wl_client_destroy(first);
    EXPECT_TRUE(m_registry.hasClients(seat(0)));
    EXPECT_EQ(m_registry.seatFor(second), seat(0));
    wl_client_destroy(second);
    EXPECT_FALSE(m_registry.hasClients(seat(0)));
}

TEST_F(CSeatClientRegistryTest, RetiredAndReplacementSeatIdentitiesRemainSeparate) {
    const auto retiredClient     = newClient();
    const auto replacementClient = newClient();
    ASSERT_NE(retiredClient, nullptr);
    ASSERT_NE(replacementClient, nullptr);
    m_registry.add(retiredClient, seat(0));
    m_registry.add(replacementClient, seat(1));
    wl_client_destroy(retiredClient);
    EXPECT_EQ(m_registry.seatFor(retiredClient), nullptr);
    EXPECT_EQ(m_registry.seatFor(replacementClient), seat(1));
    EXPECT_TRUE(m_registry.hasClients(seat(1)));
}

TEST_F(CSeatClientRegistryTest, DisplayClientCleanupClearsAllRegistrations) {
    for (size_t i = 0; i < 16; ++i) {
        const auto client = newClient();
        ASSERT_NE(client, nullptr);
        m_registry.add(client, seat(i % 2));
    }
    wl_display_destroy_clients(m_display.get());
    EXPECT_FALSE(m_registry.hasClients(seat(0)));
    EXPECT_FALSE(m_registry.hasClients(seat(1)));
}

TEST_F(CSeatClientRegistryTest, RegistryCanBeDestroyedBeforeItsClients) {
    const auto client = newClient();
    ASSERT_NE(client, nullptr);
    {
        CSeatClientRegistry temporary;
        temporary.add(client, seat(0));
        EXPECT_EQ(temporary.seatFor(client), seat(0));
    }
    m_registry.add(client, seat(1));
    EXPECT_EQ(m_registry.seatFor(client), seat(1));
    wl_client_destroy(client);
    EXPECT_FALSE(m_registry.hasClients(seat(1)));
}

TEST_F(CSeatClientRegistryTest, UnknownAndNullClientsHaveNoConnectionIdentity) {
    const auto client = newClient();
    ASSERT_NE(client, nullptr);
    EXPECT_EQ(m_registry.idFor(client), 0);
    EXPECT_EQ(m_registry.idFor(nullptr), 0);
    EXPECT_EQ(m_registry.lastId(), 0);
    m_registry.track(nullptr);
    m_registry.add(nullptr, seat(0));
    m_registry.add(client, nullptr);
    EXPECT_EQ(m_registry.idFor(client), 0);
    EXPECT_EQ(m_registry.lastId(), 0);
}

TEST_F(CSeatClientRegistryTest, ConnectionIdentityIsImmutableAndNeverReused) {
    const auto first = newClient();
    ASSERT_NE(first, nullptr);
    m_registry.track(first);
    const auto firstId = m_registry.idFor(first);
    ASSERT_GT(firstId, 0);
    m_registry.track(first);
    m_registry.add(first, seat(0));
    EXPECT_EQ(m_registry.idFor(first), firstId);
    EXPECT_EQ(m_registry.lastId(), firstId);
    wl_client_destroy(first);
    EXPECT_EQ(m_registry.idFor(first), 0);
    EXPECT_EQ(m_registry.lastId(), firstId);

    const auto second = newClient();
    ASSERT_NE(second, nullptr);
    m_registry.track(second);
    EXPECT_GT(m_registry.idFor(second), firstId);
    EXPECT_EQ(m_registry.lastId(), m_registry.idFor(second));
}

TEST_F(CSeatClientRegistryTest, TrackingDoesNotSelectAPreferredSeat) {
    const auto client = newClient();
    ASSERT_NE(client, nullptr);
    m_registry.track(client);
    const auto id = m_registry.idFor(client);
    EXPECT_EQ(m_registry.seatFor(client), nullptr);
    EXPECT_FALSE(m_registry.hasClients(seat(0)));
    EXPECT_FALSE(m_registry.hasClients(nullptr));
    m_registry.add(client, seat(0));
    m_registry.add(client, seat(1));
    EXPECT_EQ(m_registry.idFor(client), id);
    EXPECT_EQ(m_registry.seatFor(client), seat(0));
    EXPECT_TRUE(m_registry.hasClients(seat(0)));
    EXPECT_FALSE(m_registry.hasClients(seat(1)));
    wl_client_destroy(client);
    EXPECT_EQ(m_registry.idFor(client), 0);
    EXPECT_FALSE(m_registry.hasClients(seat(0)));
}

TEST_F(CSeatClientRegistryTest, ConstructorTracksExistingDisplayClients) {
    const auto first  = newClient();
    const auto second = newClient();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    CSeatClientRegistry registry(m_display.get());
    EXPECT_GT(registry.idFor(first), 0);
    EXPECT_GT(registry.idFor(second), 0);
    EXPECT_NE(registry.idFor(first), registry.idFor(second));
    EXPECT_EQ(registry.lastId(), 2);
    EXPECT_EQ(registry.seatFor(first), nullptr);
    EXPECT_EQ(registry.seatFor(second), nullptr);
}

TEST_F(CSeatClientRegistryTest, DisplayCreatedListenerTracksClientsBeforePreferredSeatBinding) {
    CSeatClientRegistry registry(m_display.get());
    EXPECT_EQ(registry.lastId(), 0);
    const auto first = newClient();
    ASSERT_NE(first, nullptr);
    const auto firstId = registry.idFor(first);
    ASSERT_GT(firstId, 0);
    EXPECT_EQ(registry.seatFor(first), nullptr);
    registry.add(first, seat(0));
    EXPECT_EQ(registry.idFor(first), firstId);
    EXPECT_EQ(registry.seatFor(first), seat(0));
    const auto second = newClient();
    ASSERT_NE(second, nullptr);
    EXPECT_GT(registry.idFor(second), firstId);
    EXPECT_EQ(registry.seatFor(second), nullptr);
    EXPECT_FALSE(registry.hasClients(nullptr));
    wl_client_destroy(first);
    EXPECT_EQ(registry.idFor(first), 0);
    EXPECT_FALSE(registry.hasClients(seat(0)));
    EXPECT_GT(registry.idFor(second), firstId);
}

TEST_F(CSeatClientRegistryTest, DisplayCleanupPreservesConnectionSequence) {
    CSeatClientRegistry registry(m_display.get());
    const auto          client = newClient();
    ASSERT_NE(client, nullptr);
    registry.add(client, seat(0));
    const auto lastId = registry.lastId();
    wl_display_destroy_clients(m_display.get());
    EXPECT_EQ(registry.idFor(client), 0);
    EXPECT_EQ(registry.lastId(), lastId);
    EXPECT_FALSE(registry.hasClients(seat(0)));
    const auto replacement = newClient();
    ASSERT_NE(replacement, nullptr);
    EXPECT_GT(registry.idFor(replacement), lastId);
}

TEST_F(CSeatClientRegistryTest, RegistryDetachesDisplayAndClientListenersBeforeDestruction) {
    const auto existing = newClient();
    ASSERT_NE(existing, nullptr);
    {
        CSeatClientRegistry registry(m_display.get());
        EXPECT_GT(registry.idFor(existing), 0);
        const auto created = newClient();
        ASSERT_NE(created, nullptr);
        EXPECT_GT(registry.idFor(created), registry.idFor(existing));
        registry.add(created, seat(0));
    }
    const auto later = newClient();
    ASSERT_NE(later, nullptr);
    wl_display_destroy_clients(m_display.get());
    EXPECT_EQ(m_registry.lastId(), 0);
}

TEST_F(CSeatClientRegistryTest, DisplayCanBeDestroyedBeforeItsRegistry) {
    std::unique_ptr<wl_display, decltype(&wl_display_destroy)> display{wl_display_create(), wl_display_destroy};
    ASSERT_NE(display, nullptr);
    CSeatClientRegistry registry(display.get());
    const auto          client = newClient(display.get());
    ASSERT_NE(client, nullptr);
    registry.add(client, seat(0));
    const auto lastId = registry.lastId();
    wl_display_destroy_clients(display.get());
    display.reset();
    EXPECT_EQ(registry.idFor(client), 0);
    EXPECT_EQ(registry.lastId(), lastId);
    EXPECT_FALSE(registry.hasClients(seat(0)));
}

TEST_F(CSeatClientRegistryTest, RemovalEmitsImmutableIdentityBeforeErasingTheClient) {
    const auto client = newClient();
    ASSERT_NE(client, nullptr);
    m_registry.add(client, seat(0));
    const auto            id = m_registry.idFor(client);
    std::vector<uint64_t> removed;
    auto                  listener = m_registry.m_events.removed.listen([&](uint64_t identity) {
        removed.emplace_back(identity);
        EXPECT_EQ(identity, id);
        EXPECT_EQ(m_registry.idFor(client), identity);
        EXPECT_EQ(m_registry.seatFor(client), seat(0));
    });
    wl_client_destroy(client);
    ASSERT_EQ(removed.size(), 1);
    EXPECT_EQ(removed.front(), id);
    EXPECT_EQ(m_registry.idFor(client), 0);
    EXPECT_EQ(m_registry.lastId(), id);
    EXPECT_FALSE(m_registry.hasClients(seat(0)));
}

TEST_F(CSeatClientRegistryTest, RemovalObserversMayTrackOtherClients) {
    const auto client = newClient();
    ASSERT_NE(client, nullptr);
    m_registry.add(client, seat(0));
    std::vector<wl_client*> later;
    for (size_t i = 0; i < 64; ++i) {
        const auto next = newClient();
        ASSERT_NE(next, nullptr);
        later.emplace_back(next);
    }
    auto listener = m_registry.m_events.removed.listen([&](uint64_t) {
        for (const auto next : later)
            m_registry.track(next);
    });
    wl_client_destroy(client);
    EXPECT_EQ(m_registry.idFor(client), 0);
    EXPECT_FALSE(m_registry.hasClients(seat(0)));
    for (const auto next : later)
        EXPECT_GT(m_registry.idFor(next), 0);
}

TEST_F(CSeatClientRegistryTest, UnknownAndOrdinaryClientsAreNotManagementClients) {
    const auto client = newClient();
    ASSERT_NE(client, nullptr);
    EXPECT_FALSE(m_registry.isManagementClient(nullptr));
    EXPECT_FALSE(m_registry.isManagementClient(client));
    m_registry.addManagement(nullptr);
    m_registry.track(client);
    EXPECT_FALSE(m_registry.isManagementClient(client));
    EXPECT_EQ(m_registry.seatFor(client), nullptr);
}

TEST_F(CSeatClientRegistryTest, ManagementRoleKeepsIdentityAndCannotSelectAPreferredSeat) {
    const auto client = newClient();
    ASSERT_NE(client, nullptr);
    m_registry.track(client);
    const auto id = m_registry.idFor(client);
    m_registry.addManagement(client);
    m_registry.addManagement(client);
    m_registry.add(client, seat(0));
    EXPECT_TRUE(m_registry.isManagementClient(client));
    EXPECT_EQ(m_registry.idFor(client), id);
    EXPECT_EQ(m_registry.seatFor(client), nullptr);
    EXPECT_FALSE(m_registry.hasClients(seat(0)));
    EXPECT_EQ(m_registry.lastId(), id);
}

TEST_F(CSeatClientRegistryTest, PreferredSeatConnectionsCannotBecomeManagementClients) {
    const auto client = newClient();
    ASSERT_NE(client, nullptr);
    m_registry.add(client, seat(0));
    const auto id = m_registry.idFor(client);
    m_registry.addManagement(client);
    EXPECT_FALSE(m_registry.isManagementClient(client));
    EXPECT_EQ(m_registry.seatFor(client), seat(0));
    EXPECT_EQ(m_registry.idFor(client), id);
    EXPECT_TRUE(m_registry.hasClients(seat(0)));
}

TEST_F(CSeatClientRegistryTest, ManagementRoleIsConnectionLocalAndRemovedWithItsIdentity) {
    CSeatClientRegistry registry(m_display.get());
    const auto          management = newClient();
    const auto          ordinary   = newClient();
    ASSERT_NE(management, nullptr);
    ASSERT_NE(ordinary, nullptr);
    registry.addManagement(management);
    registry.add(ordinary, seat(0));
    const auto id       = registry.idFor(management);
    size_t     removed  = 0;
    auto       listener = registry.m_events.removed.listen([&](uint64_t identity) {
        if (identity != id)
            return;
        ++removed;
        EXPECT_TRUE(registry.isManagementClient(management));
        EXPECT_EQ(registry.idFor(management), id);
    });
    wl_client_destroy(management);
    EXPECT_EQ(removed, 1);
    EXPECT_FALSE(registry.isManagementClient(management));
    EXPECT_EQ(registry.idFor(management), 0);
    EXPECT_FALSE(registry.isManagementClient(ordinary));
    EXPECT_EQ(registry.seatFor(ordinary), seat(0));
    const auto replacement = newClient();
    ASSERT_NE(replacement, nullptr);
    EXPECT_GT(registry.idFor(replacement), id);
    EXPECT_FALSE(registry.isManagementClient(replacement));
}

TEST_F(CSeatClientRegistryTest, DisplayCleanupRevokesManagementRolesAndKeepsTheSequence) {
    CSeatClientRegistry registry(m_display.get());
    const auto          client = newClient();
    ASSERT_NE(client, nullptr);
    registry.addManagement(client);
    const auto id = registry.idFor(client);
    wl_display_destroy_clients(m_display.get());
    EXPECT_FALSE(registry.isManagementClient(client));
    EXPECT_EQ(registry.idFor(client), 0);
    EXPECT_EQ(registry.lastId(), id);
    const auto replacement = newClient();
    ASSERT_NE(replacement, nullptr);
    registry.addManagement(replacement);
    EXPECT_TRUE(registry.isManagementClient(replacement));
    EXPECT_GT(registry.idFor(replacement), id);
}

class CWaylandSocketTest : public ::testing::Test {
  protected:
    void                                                       SetUp() override;
    void                                                       TearDown() override;
    bool                                                       connectPeer(const std::filesystem::path& path);

    std::unique_ptr<wl_display, decltype(&wl_display_destroy)> m_display{nullptr, wl_display_destroy};
    std::filesystem::path                                      m_directory;
    std::vector<Hyprutils::OS::CFileDescriptor>                m_peers;
};

void CWaylandSocketTest::SetUp() {
    static std::atomic<uint64_t> nextId = 0;
    m_display.reset(wl_display_create());
    ASSERT_NE(m_display, nullptr);
    m_directory = std::filesystem::temp_directory_path() / std::format("hyprland-wayland-socket-{}-{}", getpid(), ++nextId);
    ASSERT_TRUE(std::filesystem::create_directory(m_directory));
}

void CWaylandSocketTest::TearDown() {
    if (m_display)
        wl_display_destroy_clients(m_display.get());
    std::error_code error;
    std::filesystem::remove_all(m_directory, error);
}

bool CWaylandSocketTest::connectPeer(const std::filesystem::path& path) {
    Hyprutils::OS::CFileDescriptor peer{socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!peer.isValid())
        return false;
    sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = {}};
    const auto  name    = path.string();
    if (name.size() >= sizeof(address.sun_path))
        return false;
    std::ranges::copy(name, address.sun_path);
    if (connect(peer.get(), rc<sockaddr*>(&address), sizeof(address)) < 0)
        return false;
    m_peers.emplace_back(std::move(peer));
    return true;
}

TEST_F(CWaylandSocketTest, AcceptedClientsHaveTheirRoleBeforeRegistryRequests) {
    CSeatClientRegistry registry(m_display.get());
    std::vector<bool>   visible;
    struct SFilterContext {
        CSeatClientRegistry* registry = nullptr;
        std::vector<bool>*   visible  = nullptr;
    } context{.registry = &registry, .visible = &visible};
    wl_display_set_global_filter(
        m_display.get(),
        [](const wl_client* client, const wl_global*, void* data) {
            const auto context    = sc<SFilterContext*>(data);
            const bool management = context->registry->isManagementClient(client);
            context->visible->emplace_back(management);
            return management;
        },
        &context);
    ASSERT_NE(wl_global_create(m_display.get(), &wl_output_interface, 4, nullptr, [](wl_client*, void*, uint32_t, uint32_t) { ; }), nullptr);
    wl_client*     accepted = nullptr;
    const auto     path     = m_directory / "management";
    CWaylandSocket socket(m_display.get(), path.string(), [&](wl_client* client) {
        accepted = client;
        registry.addManagement(client);
    });
    EXPECT_EQ(std::filesystem::status(path).permissions(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    ASSERT_TRUE(connectPeer(path));
    // wl_display.get_registry(new_id=2), queued before the server accepts.
    const std::array<uint32_t, 3> request = {1, (12U << 16U) | 1U, 2};
    ASSERT_EQ(send(m_peers.back().get(), request.data(), sizeof(request), 0), sizeof(request));
    ASSERT_GE(wl_event_loop_dispatch(wl_display_get_event_loop(m_display.get()), 0), 0);
    ASSERT_NE(accepted, nullptr);
    EXPECT_TRUE(registry.isManagementClient(accepted));
    EXPECT_EQ(registry.seatFor(accepted), nullptr);
    ASSERT_GE(wl_event_loop_dispatch(wl_display_get_event_loop(m_display.get()), 0), 0);
    ASSERT_FALSE(visible.empty());
    EXPECT_TRUE(std::ranges::all_of(visible, [](bool value) { return value; }));
    wl_display_set_global_filter(m_display.get(), nullptr, nullptr);
}

TEST_F(CWaylandSocketTest, StopRemovesTheSocketWithoutDestroyingExistingClients) {
    CSeatClientRegistry registry(m_display.get());
    wl_client*          accepted = nullptr;
    const auto          path     = m_directory / "seat";
    CWaylandSocket      socket(m_display.get(), path.string(), [&](wl_client* client) {
        accepted = client;
        registry.addManagement(client);
    });
    ASSERT_TRUE(connectPeer(path));
    ASSERT_GE(wl_event_loop_dispatch(wl_display_get_event_loop(m_display.get()), 0), 0);
    ASSERT_NE(accepted, nullptr);
    const auto id = registry.idFor(accepted);
    socket.stop();
    socket.stop();
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_FALSE(connectPeer(path));
    EXPECT_TRUE(registry.isManagementClient(accepted));
    EXPECT_EQ(registry.idFor(accepted), id);
    wl_client_destroy(accepted);
    EXPECT_EQ(registry.idFor(accepted), 0);
}

TEST_F(CWaylandSocketTest, DestructionUnlinksAndDetachesTheDisplayListener) {
    const auto path = m_directory / "seat";
    {
        CWaylandSocket socket(m_display.get(), path.string(), [](wl_client*) { ; });
        EXPECT_TRUE(std::filesystem::exists(path));
    }
    EXPECT_FALSE(std::filesystem::exists(path));
    m_display.reset();
}

TEST_F(CWaylandSocketTest, DisplayMayBeDestroyedBeforeTheSocket) {
    const auto     path = m_directory / "seat";
    CWaylandSocket socket(m_display.get(), path.string(), [](wl_client*) { ; });
    EXPECT_TRUE(std::filesystem::exists(path));
    m_display.reset();
    EXPECT_FALSE(std::filesystem::exists(path));
    socket.stop();
}

TEST_F(CWaylandSocketTest, PathCollisionDoesNotUnlinkAnotherListener) {
    const auto     path = m_directory / "seat";
    CWaylandSocket first(m_display.get(), path.string(), [](wl_client*) { ; });
    EXPECT_THROW(CWaylandSocket(m_display.get(), path.string(), [](wl_client*) { ; }), std::system_error);
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(connectPeer(path));
}

TEST_F(CWaylandSocketTest, InvalidArgumentsDoNotCreateSocketNodes) {
    const auto path = m_directory / "seat";
    EXPECT_THROW(CWaylandSocket(nullptr, path.string(), [](wl_client*) { ; }), std::invalid_argument);
    EXPECT_THROW(CWaylandSocket(m_display.get(), path.string(), {}), std::invalid_argument);
    EXPECT_THROW(CWaylandSocket(m_display.get(), std::string(108, 'x'), [](wl_client*) { ; }), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path));
}
