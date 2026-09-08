#pragma once

#include "api/service.h"

namespace barista::drh
{
// Radio/platform backends implement this narrow interface. SessionServer will
// become the portable replacement for drcd::ServerCore during migration.
class RadioBackend
{
public:
    virtual ~RadioBackend() = default;
    virtual api::SessionStatus Status() const = 0;
    virtual std::optional<api::Error> Start(const api::StartSessionRequest& request) = 0;
    virtual std::optional<api::Error> Pair(const api::PairRequest& request) = 0;
    virtual std::optional<api::Error> Stop() = 0;
};
}
