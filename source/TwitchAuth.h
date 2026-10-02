#pragma once
// TwitchAuth.h - signing in to RPCN with a Twitch account instead of an npid and a password.
//
// RPCN grew a second login path (RipleyTom/rpcn, docs/twitch_auth.md): the OAuth DEVICE CODE
// grant. The player is shown a short code, enters it on twitch.tv in a browser, and the server -
// which does all the talking to Twitch itself, so no client ever holds a Twitch token - answers
// with an npid and a LOGIN TOKEN. From then on the ordinary Login command is given that token in
// place of the password. The browser trip happens ONCE, not once per session, which is the only
// reason this is usable at all from a game.
//
// WHY IT MATTERS HERE more than it does for a PlayStation emulator: "make an account on a
// PlayStation Network emulation server" is a strange first step for a player whose only
// PlayStation-anything is YAMP. Account.h softened that by registering one from inside YAMP; this
// removes the account from the question entirely for anyone who already has a Twitch login.
//
// SHAPED LIKE AccountMaker, and for its reasons: its own connection, before and without any
// login, never touching the session's state - a session that is IDLE stays IDLE while someone
// signs in. It differs in the one way that drives the whole class: this is not one request and
// one reply but a POLL LOOP that may legitimately run for half an hour, because what it waits for
// is a human being finding a browser.
//
// THE EXCHANGE:
//
//     TwitchDeviceStart      ->  flow_id, user_code, verification_uri, expires_in, interval
//     (show the code, open the page, then every `interval` seconds)
//     TwitchDevicePoll(flow) ->  TwitchAuthPending ... until it is not
//     on NoError             ->  npid, online_name, avatar_url, login_token
//
// The device code never leaves the server; a client only ever holds the opaque flow id.
//
// NONE of the Twitch error codes close the connection, deliberately, so a server that refuses
// this leaves a player who can still log in with a password. The one case that DOES close it is a
// server too old to know the command at all: it answers Malformed and hangs up. That is why a
// disconnect while Starting is read as Unsupported rather than as a failure - "this server has no
// Twitch login" is not an error a player can act on, it is a button that should not be offered.
// A third server stays connected and answers Invalid(2): one whose CommandType HAS that id, but a
// different command that an unauthentified client may not send. That is the official server,
// np.rpcs3.net (seen 2026-09-23, asking 63 on protocol 30). A server with Twitch never answers the start with Invalid.

#include <stdint.h>

#include "RpcnClient.h"

namespace yampnet
{
    class TwitchLogin
    {
    public:
        enum class State
        {
            Idle,
            Starting,     // connected; the device code has been asked for
            Waiting,      // UserCode()/VerificationUri() are live and the player is in a browser
            Done,         // Npid() and LoginToken() are what the settings should now hold
            Failed,
            // This server has no Twitch sign-in - it is either not configured for one or predates
            // the commands. Separate from Failed because the right response is to fall back to
            // the password boxes, not to show an error about something nobody did wrong.
            Unsupported,
        };

        // Connects and asks for a device code. False means it could not get that far; everything
        // after that is reported through GetState().
        bool Start(const char* server, uint16_t port, const char* fingerprint_hex);

        // Pumps the flow. Cheap and safe to call when idle, and REQUIRED while Waiting: nothing
        // else drives the poll timer, so a flow that is not updated never finishes.
        void Update();

        // Gives up on a flow in progress.
        void Cancel();
        void Reset();

        State GetState() const { return m_state; }

        // Valid while Waiting. The URI already carries the code in its query string, so opening
        // it is enough on its own - but SHOW the code as well, both as the fallback for a browser
        // that would not launch and so the player can check the page is asking about this code
        // and not one from an abandoned attempt.
        const char* UserCode() const { return m_code.user_code; }
        const char* VerificationUri() const { return m_code.verification_uri; }
        // Seconds until the code dies and the sign-in has to be started again. 0 when not Waiting.
        uint32_t SecondsRemaining() const;

        // Valid once Done. The login token goes where the PASSWORD goes - it is what Login is
        // given from every session onwards - and the verification token field stays empty,
        // because a login that arrives on a Twitch token is never checked against one.
        //
        // A FRESH TOKEN IS ISSUED BY EVERY COMPLETED FLOW, which invalidates the one before it.
        // So a stored token is not a second credential to keep alongside a password; it is the
        // credential, and signing in again on another machine ends this one's session.
        const char* Npid() const { return m_creds.npid; }
        // The server's online name for the account, which follows the Twitch display name. Worth
        // showing next to the npid, because the npid may have been truncated to 16 characters or
        // given a numeric suffix to make it free, and then it is not the name the player knows.
        const char* OnlineName() const { return m_creds.online_name; }
        const char* LoginToken() const { return m_creds.login_token; }

        const char* LastError() const { return m_error; }

    private:
        // Terminal states go through here. `fmt` may be null, which clears the message.
        void Finish(State state, const char* fmt, ...);
        bool SendPoll();
        void OnStartReply(const RpcnPacket& pkt);
        void OnPollReply(const RpcnPacket& pkt);

        RpcnClient m_client;
        State m_state = State::Idle;
        RpcnClient::TwitchDeviceCode m_code = {};
        RpcnClient::TwitchCredentials m_creds = {};
        uint64_t m_pending = 0;             // packet id of the request in flight, 0 if none
        uint64_t m_reply_deadline_ms = 0;   // that request must be answered by then
        uint64_t m_flow_deadline_ms = 0;    // the whole device code expires then
        uint64_t m_next_poll_ms = 0;
        bool m_start_sent = false;          // the start went out (it waits for the greeting)
        uint32_t m_interval_s = 5;          // as the server gave it, widened on TwitchAuthSlowDown
        // Signing in to np.rpcs3.net, which has no Twitch sign-in. It is still ASKED rather than
        // refused here, so the day it gains one needs no new plugin; this only lets the refusal
        // say what to do instead.
        bool m_official = false;
        char m_error[256] = {};
    };
}
