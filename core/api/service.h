#pragma once

#include "types.h"

#include <memory>
#include <vector>

namespace barista::api
{
// This is the platform-neutral control boundary. D-Bus, Unix sockets and the
// future VM TCP endpoint adapt requests to this interface; none is canonical.
class SessionService
{
public:
    virtual ~SessionService() = default;

    virtual SessionStatus GetStatus() const = 0;
    virtual std::vector<GamePad> ListGamePads() const = 0;
    virtual std::optional<Error> PrepareSystem() = 0;
    virtual std::optional<Error> StartSession(const StartSessionRequest& request) = 0;
    virtual std::optional<Error> Pair(const PairRequest& request) = 0;
    virtual std::optional<Error> StopSession() = 0;
    virtual std::optional<Error> RenameGamePad(const RenameGamePadRequest& request) = 0;
    virtual std::optional<Error> RemoveGamePad(const RemoveGamePadRequest& request) = 0;
};

using SessionServicePtr = std::shared_ptr<SessionService>;
}
