#pragma once
// Account.h - registering an RPCN account, which is the step before everything else in this
// plugin can be used at all.
//
// RPCN's Create command runs on its OWN connection and before any login, so this deliberately
// does not go through RpcnTransport: that class is a logged-in session with discovery, a room, a
// signaling socket and a peer, none of which exist yet and none of which a sign-up should be able
// to disturb. AccountMaker is a connection, one request and one reply.
//
// Asynchronous like the rest of the plugin - Start() returns immediately and Update() is called
// every frame until State() leaves Working. It carries its own timeout, because the failure this
// is most likely to meet in the wild is a server that accepts the TCP connection and then says
// nothing, and "the button did nothing, for ever" is not an outcome a UI can explain.

#include <stdint.h>

#include "RpcnClient.h"

namespace yampnet
{
    class AccountMaker
    {
    public:
        enum class State
        {
            Idle,
            Working,
            Created,
            Failed,
        };

        struct Fields
        {
            const char* npid = nullptr;
            const char* password = nullptr;
            const char* email = nullptr;
            const char* online_name = nullptr;   // null/empty = npid
            const char* avatar_url = nullptr;    // null/empty = kDefaultAvatarUrl
        };

        // The server insists on a non-empty avatar, and a player has no reason to have one. This
        // says where the account came from rather than inventing a picture that does not exist.
        static constexpr const char* kDefaultAvatarUrl = "https://github.com/biggestsonicfan/YAMP";

        // Connects and sends Create. Returns false (and sets State to Failed) if the connection or
        // the request could not be made at all; everything after that is reported through State().
        bool Start(const char* server, uint16_t port, const char* fingerprint_hex,
                   const Fields& fields);

        // Pumps the reply. Cheap and safe to call when idle.
        void Update();

        State GetState() const { return m_state; }
        const char* LastError() const { return m_error; }

        void Reset();

    private:
        void Fail(const char* fmt, ...);
        void Finish(State state);

        RpcnClient m_client;
        State m_state = State::Idle;
        uint64_t m_pending = 0;      // packet id of the Create request in flight
        uint64_t m_deadline_ms = 0;
        char m_error[256] = {};
    };
}
