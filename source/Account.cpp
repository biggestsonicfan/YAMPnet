#include "Account.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace yampnet
{
    namespace
    {
        // Long enough for a TLS handshake and a round trip on a bad line, short enough that a
        // silent server does not leave a settings page saying "working" until YAMP is restarted.
        constexpr uint64_t kReplyTimeoutMs = 15000;

        // What the server's ErrorType means for THIS request. The generic codes say nothing a
        // player can act on ("InvalidInput") and the same byte means something different after
        // every command, so it is translated here rather than shown raw.
        const char* CreateErrorText(RpcnError error)
        {
            switch (error)
            {
            case RpcnError::Malformed:
                return "the server rejected the sign-up as malformed - one of the fields is empty "
                       "or too long";
            case RpcnError::CreationExistingUsername:
            // Current RPCN answers CreationExistingUsername for a name clash; Invalid is what
            // older builds sent, and it means nothing else after Create.
            case RpcnError::Invalid:
                return "that account name is already taken on this server";
            case RpcnError::CreationExistingEmail:
                return "there is already an account on this server with that e-mail address";
            case RpcnError::CreationBannedEmailProvider:
                return "this server does not accept accounts from that e-mail provider - try an "
                       "address somewhere else";
            case RpcnError::InvalidInput:
                return "the server refused these details: the name must be 3-16 characters of "
                       "letters, digits, '-' or '_', and the e-mail address must be a real one";
            case RpcnError::TooSoon:
                return "too many sign-ups from this address recently - wait a while and try again";
            case RpcnError::DbFail:
                return "the server's database did not answer";
            default:
                return "the server refused the sign-up";
            }
        }

        // The same for SendToken, where every code means something different again - Invalid is
        // not a name clash here but "this server has no tokens at all".
        const char* ResendErrorText(RpcnError error)
        {
            switch (error)
            {
            case RpcnError::Invalid:
                return "this server does not verify accounts by e-mail, so it has no token to "
                       "send - leave the token box empty and log in";
            case RpcnError::TooSoon:
                return "a token was already e-mailed for this account in the last 24 hours - the "
                       "server will not send another until then";
            case RpcnError::LoginError:
                return "the server did not accept that account name and password";
            case RpcnError::EmailFail:
                return "the server could not send the e-mail";
            case RpcnError::Malformed:
                return "the server could not read the request";
            case RpcnError::DbFail:
                return "the server's database did not answer";
            default:
                return "the server refused to send the token";
            }
        }
    }

    void AccountMaker::Fail(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        vsnprintf(m_error, sizeof(m_error), fmt, args);
        va_end(args);
        Finish(State::Failed);
    }

    void AccountMaker::Finish(State state)
    {
        m_state = state;
        m_pending = 0;
        m_deadline_ms = 0;
        // The connection has done its one job. Holding it open would leave an unauthenticated
        // socket that the server drops after ten seconds anyway.
        m_client.Disconnect();
    }

    void AccountMaker::Reset()
    {
        m_client.Disconnect();
        m_state = State::Idle;
        m_job = Job::Create;
        m_pending = 0;
        m_deadline_ms = 0;
        m_error[0] = '\0';
    }

    bool AccountMaker::Begin(Job job, const char* server, uint16_t port,
                             const char* fingerprint_hex)
    {
        Reset();
        m_job = job;

        if (!server || !*server)
        {
            Fail(job == Job::Create ? "no server to create the account on"
                                    : "no server to ask for a token");
            return false;
        }

        CertFingerprint pin;
        if (!ResolveServerPin(server, fingerprint_hex, &pin))
        {
            Fail("bad certificate fingerprint");
            return false;
        }

        if (!m_client.Connect(server, port ? port : kRpcnDefaultPort, pin))
        {
            Fail("could not reach %s: %s", server, m_client.LastError());
            return false;
        }

        m_state = State::Working;
        m_deadline_ms = GetTickCount64() + kReplyTimeoutMs;
        return true;
    }

    bool AccountMaker::Start(const char* server, uint16_t port, const char* fingerprint_hex,
                             const Fields& fields)
    {
        // The fields are checked before anything is connected, so a blank box costs no round
        // trip and the message can name the box. Reset() first because these are failures of
        // THIS attempt: the state a UI reads must be about what it just asked for.
        Reset();
        m_job = Job::Create;
        if (!fields.npid || !*fields.npid || !fields.password || !*fields.password)
        {
            Fail("an account name and a password are both required");
            return false;
        }
        if (!fields.email || !*fields.email)
        {
            Fail("an e-mail address is required: the server stores one for every account");
            return false;
        }

        if (!Begin(Job::Create, server, port, fingerprint_hex))
            return false;

        // Both of these are required by the server and neither is worth a form field: an online
        // name that is not the login name only confuses a player who has one account, and an
        // avatar is a URL nobody has to hand.
        const char* online_name = (fields.online_name && *fields.online_name)
                                ? fields.online_name : fields.npid;
        const char* avatar_url = (fields.avatar_url && *fields.avatar_url)
                               ? fields.avatar_url : kDefaultAvatarUrl;

        m_pending = m_client.CreateAccount(fields.npid, fields.password, online_name, avatar_url,
                                           fields.email);
        if (m_pending == 0)
        {
            // RpcnClient::CreateAccount checks the server's own field rules before sending, so
            // its message already names the offending field.
            Fail("%s", m_client.LastError());
            return false;
        }
        return true;
    }

    bool AccountMaker::StartTokenResend(const char* server, uint16_t port,
                                        const char* fingerprint_hex, const char* npid,
                                        const char* password)
    {
        Reset();
        m_job = Job::ResendToken;
        if (!npid || !*npid || !password || !*password)
        {
            Fail("the account name and password are both required to ask for a token");
            return false;
        }

        if (!Begin(Job::ResendToken, server, port, fingerprint_hex))
            return false;

        m_pending = m_client.ResendToken(npid, password);
        if (m_pending == 0)
        {
            Fail("%s", m_client.LastError());
            return false;
        }
        return true;
    }

    void AccountMaker::Update()
    {
        if (m_state != State::Working)
            return;

        const RpcnCommand expect = (m_job == Job::Create) ? RpcnCommand::Create
                                                          : RpcnCommand::SendToken;

        RpcnPacket pkt;
        while (m_client.Poll(&pkt))
        {
            // type 1 is a reply; the ServerInfo greeting (type 0) arrives first and says nothing
            // about this request.
            if (pkt.type != 1 || static_cast<RpcnCommand>(pkt.command) != expect)
                continue;
            if (m_pending != 0 && pkt.packet_id != m_pending)
                continue;

            if (pkt.error != RpcnError::NoError)
            {
                Fail("%s (ErrorType=%u)",
                     (m_job == Job::Create) ? CreateErrorText(pkt.error)
                                            : ResendErrorText(pkt.error),
                     static_cast<unsigned>(pkt.error));
                return;
            }
            m_error[0] = '\0';
            Finish(State::Done);
            return;
        }

        if (!m_client.IsConnected())
        {
            Fail("the server closed the connection without answering");
            return;
        }

        if (m_deadline_ms != 0 && GetTickCount64() > m_deadline_ms)
            Fail("the server did not answer within %u seconds",
                 static_cast<unsigned>(kReplyTimeoutMs / 1000));
    }
}
