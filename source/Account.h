#pragma once
// Account.h - registering an RPCN account, which is the step before everything else in this
// plugin can be used at all, and having its verification token mailed again.
//
// RPCN's Create and SendToken commands run on their OWN connection and before any login, so this
// deliberately does not go through RpcnTransport: that class is a logged-in session with
// discovery, a room, a signaling socket and a peer, none of which exist yet and none of which a
// sign-up should be able to disturb. AccountMaker is a connection, one request and one reply.
//
// BOTH JOBS LIVE HERE because they are the same shape - connect, ask, read one answer, hang up -
// and because they are two halves of one story: a server with e-mail validation switched on mails
// a token when Create succeeds, and login refuses the account until that token comes back. A
// player who never received the message would otherwise have an account they can never use.
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
            Done,        // the server accepted it; GetJob() says WHAT it accepted
            Failed,
        };

        // Which of the two one-shot commands is in flight, so a caller (and an error message) can
        // tell "the account exists now" from "the token is on its way".
        enum class Job
        {
            Create,
            ResendToken,
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

        // Asks the server to mail the account's token again. Same connection, timeout and
        // reporting as Start(); it needs only the login name and password, because SendToken
        // authenticates without the token - a player who has lost it could not otherwise ask.
        //
        // A server that does not verify accounts by e-mail refuses this outright, and one that
        // does refuses a second request inside 24 hours. Both come back as Failed with a message
        // saying which.
        bool StartTokenResend(const char* server, uint16_t port, const char* fingerprint_hex,
                              const char* npid, const char* password);

        // Pumps the reply. Cheap and safe to call when idle.
        void Update();

        State GetState() const { return m_state; }
        Job GetJob() const { return m_job; }
        const char* LastError() const { return m_error; }

        void Reset();

    private:
        void Fail(const char* fmt, ...);
        void Finish(State state);
        // Connect + timeout + Reset, shared by the two jobs. False means it already failed.
        bool Begin(Job job, const char* server, uint16_t port, const char* fingerprint_hex);

        RpcnClient m_client;
        State m_state = State::Idle;
        Job m_job = Job::Create;
        uint64_t m_pending = 0;      // packet id of the request in flight
        uint64_t m_deadline_ms = 0;
        char m_error[256] = {};
    };
}
