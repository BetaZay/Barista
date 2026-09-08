#pragma once
#include "api/controller.h"
namespace barista {
class UinputOutput final : public ControllerOutput {
public:
    ~UinputOutput() override { Stop(); }
    bool Start(std::string& error) override;
    bool Submit(const ControllerState& state) override;
    void Stop() override;
private:
    int m_fd = -1;
};
}
