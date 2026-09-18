#include "TwitchAuth.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace yampnet
{
    namespace
    {
        // The device request is a round trip to Twitch on the server's side, so it is allowed
        // longer than AccountMaker's plain database hits - but not so long that a silent server
        // leaves a settings page saying "working" until YAMP is restarted.
        constexpr uint64_t kStartTimeoutMs = 20000;
        // Each poll is another server-side round trip to Twitch, whose own HTTP timeout there is
        // 15 seconds, so anything past this is the server having stopped rather than being slow.
        constexpr uint64_t kPollTimeoutMs = 45000;
        // Only reached if the server hands out a nonsensical one; it clamps its own to 1..60.
        constexpr uint32_t kDefaultIntervalS = 5;
        constexpr uint32_t kMaxIntervalS = 60;
        // What a TwitchAuthSlowDown adds. The server only sends it when we polled inside the
        // interval it gave, so a second is enough to walk out of the window, and it accumulates
        // if we somehow keep doing it.
        constexpr uint32_t kSlowDownStepS = 1;
        // Bounds on the lifetime the server reports. Zero would expire the flow before the first
        // poll, and Twitch's own codes last 30 minutes, so anything longer is not a code we could
        // still be holding by the time it mattered.
        constexpr uint32_t kMinFlowSeconds = 30;
        constexpr uint32_t kMaxFlowSeconds = 30 * 60;

        // What the server's ErrorType means for THIS exchange. The generic codes say nothing a
        // player can act on and every command gives the same byte a different meaning, so it is
        // translated here rather than shown raw - as it is for Create and SendToken.
        const char* TwitchErrorText(RpcnError error)
        {
            switch (error)
            {
            case RpcnError::TwitchAuthExpired:
                return "the code expired, or was already used - start the sign-in again";
            case RpcnError::TwitchAuthDenied:
                return "the authorisation was refused on twitch.tv";
            case RpcnError::TwitchAuthError:
                return "the server could not reach Twitch - try again in a moment";
            case RpcnError::TwitchDisabled:
                return "this server does not offer Twitch sign-in";
            // The account this Twitch identity resolved to is already linked to a DIFFERENT
            // Twitch id, which the server never re-links - that is what stops one Twitch user
            // taking over another one's account by renaming.
            case RpcnError::Unauthorized:
                return "that Twitch login belongs to an account this server will not hand over";
            case RpcnError::CreationExistingUsername:
                return "the server could not find a free account name for that Twitch login";
            case RpcnError::CreationExistingEmail:
                return "the server already has an account registered against that Twitch login";
            case RpcnError::DbFail:
                return "the server's database did not answer";
            case RpcnError::Malformed:
                return "the server could not read the request";
            default:
                return "the server refused the Twitch sign-in";
            }
        }
    }

    void TwitchLogin::Finish(State state, const char* fmt, ...)
    {
        if (fmt)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(m_error, sizeof(m_error), fmt, args);
            va_end(args);
        }
        else
        {
            m_error[0] = '\0';
        }

        m_state = state;
        m_pending = 0;
        m_reply_deadline_ms = 0;
        m_flow_deadline_ms = 0;
        m_next_poll_ms = 0;
        // The connection has done its one job, whichever way it went. Holding it open would leave
        // an unauthentified socket that the server drops on its own anyway.
        m_client.Disconnect();
    }

    void TwitchLogin::Reset()
    {
        m_client.Disconnect();
        m_state = State::Idle;
        m_code = RpcnClient::TwitchDeviceCode{};
        m_creds = RpcnClient::TwitchCredentials{};
        m_pending = 0;
        m_reply_deadline_ms = 0;
        m_flow_deadline_ms = 0;
        m_next_poll_ms = 0;
        m_interval_s = kDefaultIntervalS;
        m_error[0] = '\0';
    }

    void TwitchLogin::Cancel()
    {
        // The server's copy of the flow is left to expire on its own: there is no command to
        // withdraw one, and it costs a few hundred bytes for at most half an hour. Dropping the
        // connection is the whole of what a client can do about it, and is enough.
        Reset();
    }

    bool TwitchLogin::Start(const char* server, uint16_t port, const char* fingerprint_hex)
    {
        Reset();

        if (!server || !*server)
        {
            Finish(State::Failed, "no server to sign in to");
            return false;
        }

        CertFingerprint pin;
        if (fingerprint_hex && *fingerprint_hex && !pin.FromHex(fingerprint_hex))
        {
            Finish(State::Failed, "bad certificate fingerprint");
            return false;
        }

        if (!m_client.Connect(server, port ? port : kRpcnDefaultPort, pin))
        {
            Finish(State::Failed, "could not reach %s: %s", server, m_client.LastError());
            return false;
        }

        m_pending = m_client.TwitchDeviceStart();
        if (m_pending == 0)
        {
            Finish(State::Failed, "%s", m_client.LastError());
            return false;
        }

        m_state = State::Starting;
        m_reply_deadline_ms = GetTickCount64() + kStartTimeoutMs;
        return true;
    }

    void TwitchLogin::OnStartReply(const RpcnPacket& pkt)
    {
        m_pending = 0;

        // Not configured on this server. Not a failure: the player asked for a door that is not
        // there, and the password boxes still are.
        if (pkt.error == RpcnError::TwitchDisabled)
        {
            Finish(State::Unsupported, "this server does not offer Twitch sign-in");
            return;
        }
        // An RPCN from before the feature does not know command 63 at all and rejects it as
        // Malformed (then hangs up, which Update() catches as the same answer).
        if (pkt.error == RpcnError::Malformed)
        {
            Finish(State::Unsupported, "this server is too old to know about Twitch sign-in");
            return;
        }
        if (pkt.error != RpcnError::NoError)
        {
            Finish(State::Failed, "%s (ErrorType=%u)", TwitchErrorText(pkt.error),
                   static_cast<unsigned>(pkt.error));
            return;
        }

        if (!RpcnClient::ParseTwitchDeviceCode(pkt.payload, pkt.payload_size, &m_code))
        {
            Finish(State::Failed, "the server's device code reply could not be read");
            return;
        }

        m_interval_s = m_code.interval ? m_code.interval : kDefaultIntervalS;
        if (m_interval_s > kMaxIntervalS)
            m_interval_s = kMaxIntervalS;

        uint32_t lifetime = m_code.expires_in;
        if (lifetime < kMinFlowSeconds) lifetime = kMinFlowSeconds;
        if (lifetime > kMaxFlowSeconds) lifetime = kMaxFlowSeconds;

        const uint64_t now = GetTickCount64();
        m_flow_deadline_ms = now + static_cast<uint64_t>(lifetime) * 1000;
        // The server starts its rate-limit window when it creates the flow, so the FIRST poll
        // waits an interval like every other one - going straight out would only earn a SlowDown.
        m_next_poll_ms = now + static_cast<uint64_t>(m_interval_s) * 1000;
        m_reply_deadline_ms = 0;
        m_error[0] = '\0';
        m_state = State::Waiting;
    }

    void TwitchLogin::OnPollReply(const RpcnPacket& pkt)
    {
        m_pending = 0;
        m_reply_deadline_ms = 0;

        const uint64_t now = GetTickCount64();

        switch (pkt.error)
        {
        // The player has not finished in the browser yet. This is the normal answer, and it stays
        // the answer for as long as they take.
        case RpcnError::TwitchAuthPending:
            m_next_poll_ms = now + static_cast<uint64_t>(m_interval_s) * 1000;
            return;

        case RpcnError::TwitchAuthSlowDown:
            // We polled inside the window the server gave us. Widen it rather than simply waiting
            // again, so a clock that disagrees slightly here does not spend the whole flow being
            // told off instead of asking.
            if (m_interval_s < kMaxIntervalS)
                m_interval_s += kSlowDownStepS;
            m_next_poll_ms = now + static_cast<uint64_t>(m_interval_s) * 1000;
            return;

        case RpcnError::NoError:
            break;

        // Everything else ends the flow, including TwitchDisabled: a server that turned the
        // feature off underneath us has no answer left to give, and a device code cannot be
        // redeemed twice, so there is nothing left to retry with.
        default:
            Finish(State::Failed, "%s (ErrorType=%u)", TwitchErrorText(pkt.error),
                   static_cast<unsigned>(pkt.error));
            return;
        }

        if (!RpcnClient::ParseTwitchCredentials(pkt.payload, pkt.payload_size, &m_creds))
        {
            Finish(State::Failed, "the server's sign-in reply could not be read");
            return;
        }

        Finish(State::Done, nullptr);
    }

    bool TwitchLogin::SendPoll()
    {
        m_pending = m_client.TwitchDevicePoll(m_code.flow_id);
        if (m_pending == 0)
        {
            Finish(State::Failed, "could not ask the server about the sign-in: %s",
                   m_client.LastError());
            return false;
        }
        m_reply_deadline_ms = GetTickCount64() + kPollTimeoutMs;
        return true;
    }

    void TwitchLogin::Update()
    {
        if (m_state != State::Starting && m_state != State::Waiting)
            return;

        RpcnPacket pkt;
        while (m_client.Poll(&pkt))
        {
            // type 1 is a reply; the ServerInfo greeting arrives first and says nothing about any
            // of this.
            if (pkt.type != 1)
                continue;
            if (m_pending != 0 && pkt.packet_id != m_pending)
                continue;

            const RpcnCommand cmd = static_cast<RpcnCommand>(pkt.command);
            if (m_state == State::Starting && cmd == RpcnCommand::TwitchDeviceStart)
                OnStartReply(pkt);
            else if (m_state == State::Waiting && cmd == RpcnCommand::TwitchDevicePoll)
                OnPollReply(pkt);
            else
                continue;

            // A terminal state has already closed the connection; Poll() would only fail.
            if (m_state != State::Starting && m_state != State::Waiting)
                return;
        }

        const uint64_t now = GetTickCount64();

        if (!m_client.IsConnected())
        {
            // A server that does not know command 63 answers Malformed and HANGS UP, so a
            // connection that dropped while the device code was in flight is not a network fault
            // - it is TwitchDisabled's answer, arrived at by an older server.
            if (m_state == State::Starting)
                Finish(State::Unsupported,
                       "this server closed the connection instead of answering, which is what an "
                       "RPCN without Twitch sign-in does");
            else
                Finish(State::Failed, "the server closed the connection during the sign-in");
            return;
        }

        if (m_state == State::Starting)
        {
            if (m_reply_deadline_ms != 0 && now > m_reply_deadline_ms)
                Finish(State::Failed, "the server did not answer within %u seconds",
                       static_cast<unsigned>(kStartTimeoutMs / 1000));
            return;
        }

        // Waiting. The code dies on its own schedule whether or not anyone is still looking at
        // the page, so this bound is the server's rather than ours.
        if (m_flow_deadline_ms != 0 && now > m_flow_deadline_ms)
        {
            Finish(State::Failed,
                   "the code expired before it was entered on twitch.tv - start the sign-in "
                   "again for a fresh one");
            return;
        }

        if (m_pending != 0)
        {
            if (m_reply_deadline_ms != 0 && now > m_reply_deadline_ms)
                Finish(State::Failed, "the server stopped answering while waiting for Twitch");
            return;
        }

        // The server relaxes an unauthentified connection's read timeout to 120 seconds once a
        // flow is running, so polling on the interval it handed out also keeps this socket alive.
        if (now >= m_next_poll_ms)
            SendPoll();
    }

    uint32_t TwitchLogin::SecondsRemaining() const
    {
        if (m_state != State::Waiting || m_flow_deadline_ms == 0)
            return 0;
        const uint64_t now = GetTickCount64();
        return (now >= m_flow_deadline_ms)
             ? 0 : static_cast<uint32_t>((m_flow_deadline_ms - now) / 1000);
    }
}
