# Barista

<p align="center"><img src="barista-logo.png" width="220" alt="Barista logo"></p>

Use a Wii U GamePad as a wireless second screen, controller, and audio device
for your Linux desktop. Barista handles pairing and streaming, with a desktop
app for managing your GamePads and connections.

Barista is experimental software and is not affiliated with Nintendo.

## Features

- Pair a real Wii U GamePad and reconnect using saved credentials.
- Stream video and audio from compatible applications, with GamePad input.
- Use the GamePad as a controller through Linux `uinput`.
- Integrate other applications through [AppHook](docs/api/README.md).

## Requirements

Real GamePad connections require **Linux and a compatible 5 GHz Wi-Fi adapter**.
Barista temporarily takes over the selected adapter, so use Ethernet or a
second adapter if you also need Internet access.

See [Wi-Fi compatibility](docs/hardware.md) for tested adapters and driver
limitations. Windows and macOS currently support the portable UI/core only.

## Getting started

1. [Build and install Barista](COMPILING.md), then open it as your normal desktop user.
2. Select your Wi-Fi adapter and confirm your country in **Settings → General**.
3. Open **GamePads → Pair a GamePad** and follow the instructions.
4. For a saved GamePad, use **Connect GamePad** on Home.

See the [user guide](docs/user-guide.md) for pairing, play modes, and running
compatible applications. If something fails, open **Settings → Support** and
check the [troubleshooting guide](docs/troubleshooting.md).

## Compatible projects

Experimental integrations are available in these forks; they are not features
of or endorsements by the upstream projects:

- [Cemu](https://github.com/BetaZay/Cemu), branch `barista-connector-prototype`
- [Azahar](https://github.com/BetaZay/Azahar)

## Project status

Pairing, basic video/audio streaming, controller input, and AppHook have been
tested with real hardware. Video can still show artifacts or fall behind;
audio can stutter, and recovery needs improvement. Touch and motion need
broader testing. Camera, microphone, and NFC support are not implemented.

## Documentation

- [Build and installation](COMPILING.md)
- [User guide](docs/user-guide.md)
- [Wi-Fi compatibility](docs/hardware.md)
- [Troubleshooting and support logs](docs/troubleshooting.md)
- [Developer API and application integration](docs/api/README.md)
