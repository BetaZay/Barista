#include "drcd/interrupt.h"
#include "drcd/media_streamer.h"
#include "drcd/session_backend.h"

#include "drc_host/pairing.h"
#include "drc_host/runtime_transport.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <random>
#include <regex>
#include <mutex>
#include <deque>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <iostream>

#include <fcntl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef DRCD_DEFAULT_HOSTAPD_BIN
#define DRCD_DEFAULT_HOSTAPD_BIN "hostapd"
#endif

namespace drcd
{
namespace
{
constexpr std::string_view kHostapdControlPath = "/var/run/hostapd";
constexpr int kDefaultWpsPinTimeoutSeconds = 600;
constexpr int kMaxWpsPinTimeoutSeconds = 3600;
constexpr std::string_view kConsoleIp = "192.168.1.10";
constexpr std::string_view kGamePadIp = "192.168.1.11";
constexpr std::string_view kBroadcastIp = "192.168.1.255";
constexpr uint16_t kSessionMtu = 1800;
constexpr uint32_t kDhcpLeaseSeconds = 3600;
constexpr std::string_view kTsfMonitorInterface = "drcdtsf";

enum class DhcpMessageType : uint8_t
{
	Discover = 1,
	Offer = 2,
	Request = 3,
	Ack = 5,
};

struct DhcpPacket
{
	uint8_t op{};
	uint8_t htype{};
	uint8_t hlen{};
	uint8_t hops{};
	uint32_t xid{};
	uint16_t secs{};
	uint16_t flags{};
	uint32_t ciaddr{};
	uint32_t yiaddr{};
	uint32_t siaddr{};
	uint32_t giaddr{};
	uint8_t chaddr[16]{};
	uint8_t sname[64]{};
	uint8_t file[128]{};
};

static_assert(sizeof(DhcpPacket) == 236);
constexpr size_t kDhcpHeaderSize = sizeof(DhcpPacket);
constexpr uint32_t kDhcpMagicCookie = 0x63825363u;

std::string Trim(std::string value)
{
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
		return std::isspace(ch) == 0;
	}));
	value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
		return std::isspace(ch) == 0;
	}).base(), value.end());
	return value;
}

void AppendCapped(std::string& dst, std::string_view chunk, size_t cap)
{
	if (chunk.empty() || cap == 0)
		return;
	dst.append(chunk.data(), chunk.size());
	if (dst.size() > cap)
		dst.erase(0, dst.size() - cap);
}

std::string TailLines(const std::string& text, size_t max_lines)
{
	if (text.empty() || max_lines == 0)
		return {};

	size_t lines = 0;
	size_t pos = text.size();
	while (pos > 0)
	{
		--pos;
		if (text[pos] == '\n')
		{
			++lines;
			if (lines >= max_lines)
			{
				++pos;
				break;
			}
		}
	}
	if (pos >= text.size())
		pos = 0;
	return Trim(text.substr(pos));
}

bool ParseBoolEnv(const char* name, bool default_value)
{
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0')
		return default_value;

	std::string text(value);
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	if (text == "1" || text == "true" || text == "yes" || text == "on")
		return true;
	if (text == "0" || text == "false" || text == "no" || text == "off")
		return false;
	return default_value;
}

std::string Quote(std::string_view text)
{
	return "'" + std::string(text) + "'";
}

int ParseIntEnv(const char* name, int default_value)
{
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0')
		return default_value;
	try
	{
		return std::stoi(value);
	}
	catch (...)
	{
		return default_value;
	}
}

std::string ReadEnvOrDefault(const char* name, std::string_view default_value)
{
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0')
		return std::string(default_value);
	return std::string(value);
}

std::vector<int> ParseChannelList(std::string_view text)
{
	std::vector<int> channels;
	std::string token;
	const auto flush_token = [&]() {
		if (token.empty())
			return;
		try
		{
			const int channel = std::stoi(token);
			if (channel > 0 && std::find(channels.begin(), channels.end(), channel) == channels.end())
				channels.push_back(channel);
		}
		catch (...)
		{
		}
		token.clear();
	};

	for (char ch : text)
	{
		if (std::isdigit(static_cast<unsigned char>(ch)))
			token.push_back(ch);
		else
			flush_token();
	}
	flush_token();
	return channels;
}

std::vector<int> BuildPairingChannelPlan(int preferred_channel, bool sweep_enabled, std::string_view channel_list_override)
{
	auto channels = ParseChannelList(channel_list_override);
	if (!channels.empty())
		return channels;

	if (preferred_channel <= 0)
		preferred_channel = 36;

	std::vector<int> plan;
	plan.push_back(preferred_channel);
	if (!sweep_enabled)
		return plan;

	// A captured North American Wii U pairing AP selected channel 165. Cover
	// every non-DFS 5 GHz channel that hostapd can bring up immediately.
	constexpr std::array<int, 9> kPairingChannels{36, 40, 44, 48, 149, 153, 157, 161, 165};
	for (const int channel : kPairingChannels)
	{
		if (std::find(plan.begin(), plan.end(), channel) == plan.end())
			plan.push_back(channel);
	}
	return plan;
}

std::string JoinChannels(const std::vector<int>& channels)
{
	std::ostringstream out;
	for (size_t i = 0; i < channels.size(); ++i)
	{
		if (i != 0)
			out << ",";
		out << channels[i];
	}
	return out.str();
}

int HexNibble(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

bool IsLocallyAdministeredMac(std::string_view mac)
{
	if (mac.size() < 2)
		return false;
	const int high = HexNibble(mac[0]);
	const int low = HexNibble(mac[1]);
	if (high < 0 || low < 0)
		return false;
	const int first_byte = (high << 4) | low;
	return (first_byte & 0x02) != 0;
}

bool ValidateInterfaceName(const std::string& iface)
{
	if (iface.empty() || iface.size() > 32)
		return false;
	return std::regex_match(iface, std::regex(R"(^[A-Za-z0-9_.:-]+$)"));
}

std::string GeneratePskHex()
{
	std::array<uint8_t, 32> psk{};
	std::random_device rd;
	std::mt19937 rng(rd());
	std::uniform_int_distribution<uint16_t> dist(0, 255);
	for (auto& b : psk)
		b = static_cast<uint8_t>(dist(rng));

	static constexpr char kHex[] = "0123456789abcdef";
	std::string out;
	out.reserve(psk.size() * 2);
	for (uint8_t b : psk)
	{
		out.push_back(kHex[(b >> 4) & 0x0f]);
		out.push_back(kHex[b & 0x0f]);
	}
	return out;
}

void AppendBigEndianU16(std::vector<uint8_t>& out, uint16_t value)
{
	out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
	out.push_back(static_cast<uint8_t>(value & 0xff));
}

void AppendWpsTlv(std::vector<uint8_t>& out, uint16_t type, const uint8_t* payload, size_t payload_len)
{
	AppendBigEndianU16(out, type);
	AppendBigEndianU16(out, static_cast<uint16_t>(payload_len));
	if (payload_len != 0)
		out.insert(out.end(), payload, payload + payload_len);
}

bool WritePairingCredentialsBlob(const std::string& runtime_ssid, const std::string& psk_hex, const drc_host::MacAddress& ap_mac, const std::string& output_path, std::string& error)
{
	if (runtime_ssid.empty() || runtime_ssid.size() > 32)
	{
		error = "invalid runtime ssid";
		return false;
	}
	if (psk_hex.size() != 64 || !std::all_of(psk_hex.begin(), psk_hex.end(), [](unsigned char ch) {
		return std::isxdigit(ch) != 0;
	}))
	{
		error = "invalid psk format";
		return false;
	}

	std::vector<uint8_t> credential;
	credential.reserve(128);
	const uint8_t network_index = 1;
	AppendWpsTlv(credential, 0x1026, &network_index, 1);
	AppendWpsTlv(credential, 0x1045, reinterpret_cast<const uint8_t*>(runtime_ssid.data()), runtime_ssid.size());
	const std::array<uint8_t, 2> auth_type_wpa2_psk{0x00, 0x20};
	AppendWpsTlv(credential, 0x1003, auth_type_wpa2_psk.data(), auth_type_wpa2_psk.size());
	const std::array<uint8_t, 2> encr_type_aes{0x00, 0x08};
	AppendWpsTlv(credential, 0x100f, encr_type_aes.data(), encr_type_aes.size());
	AppendWpsTlv(credential, 0x1027, reinterpret_cast<const uint8_t*>(psk_hex.data()), psk_hex.size());
	AppendWpsTlv(credential, 0x1020, ap_mac.bytes.data(), ap_mac.bytes.size());

	std::vector<uint8_t> blob;
	blob.reserve(credential.size() + 4);
	AppendWpsTlv(blob, 0x100e, credential.data(), credential.size());

	std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
	if (!out)
	{
		error = "failed to create credential blob";
		return false;
	}
	out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
	if (!out.good())
	{
		error = "failed while writing credential blob";
		return false;
	}
	return true;
}

bool WritePairingHostapdConfig(
	const std::string& iface,
	const drc_host::MacAddress& ap_mac,
	const std::string& pairing_ssid,
	const std::string& credential_path,
	int channel,
	bool ignore_broadcast_probe_requests,
	const std::string& output_path,
	std::string& error)
{
	if (channel <= 0)
	{
		error = "invalid channel";
		return false;
	}
	if (credential_path.empty())
	{
		error = "credential path is empty";
		return false;
	}

	const bool use5ghz = channel > 14;
	const char* hw_mode = use5ghz ? "a" : "g";

	std::ofstream conf(output_path, std::ios::trunc);
	if (!conf)
	{
		error = "failed to open hostapd pairing config";
		return false;
	}

	const std::string device_name = "WiiU" + ap_mac.to_hex_no_separator();
	conf
		<< "interface=" << iface << "\n"
		<< "driver=nl80211\n"
		<< "logger_stdout=-1\n"
		<< "logger_stdout_level=0\n"
		<< "ctrl_interface=" << kHostapdControlPath << "\n"
		<< "bssid=" << ap_mac.to_string() << "\n"
		<< "hw_mode=" << hw_mode << "\n"
		<< "channel=" << channel << "\n"
		<< "beacon_int=100\n"
		<< "dtim_period=3\n"
		<< "macaddr_acl=0\n"
		<< "auth_algs=3\n"
		<< "wmm_enabled=1\n"
		<< "uapsd_advertisement_enabled=1\n"
		<< "wmm_ac_be_acm=0\n"
		<< "wmm_ac_be_aifs=2\n"
		<< "wmm_ac_be_cwmin=4\n"
		<< "wmm_ac_be_cwmax=5\n"
		<< "wmm_ac_be_txop_limit=47\n"
		<< "wmm_ac_bk_acm=0\n"
		<< "wmm_ac_bk_aifs=7\n"
		<< "wmm_ac_bk_cwmin=4\n"
		<< "wmm_ac_bk_cwmax=10\n"
		<< "wmm_ac_bk_txop_limit=0\n"
		<< "wmm_ac_vi_acm=0\n"
		<< "wmm_ac_vi_aifs=3\n"
		<< "wmm_ac_vi_cwmin=4\n"
		<< "wmm_ac_vi_cwmax=5\n"
		<< "wmm_ac_vi_txop_limit=94\n"
		<< "wmm_ac_vo_acm=0\n"
		<< "wmm_ac_vo_aifs=3\n"
		<< "wmm_ac_vo_cwmin=4\n"
		<< "wmm_ac_vo_cwmax=5\n"
		<< "wmm_ac_vo_txop_limit=47\n"
		<< "ieee80211n=1\n"
		<< "ssid=" << pairing_ssid << "\n"
		<< "ieee8021x=1\n"
		<< "eapol_version=1\n"
		<< "eap_server=1\n"
		<< "wps_state=2\n"
		<< "skip_cred_build=1\n"
		<< "extra_cred=" << credential_path << "\n"
		<< "wps_cred_processing=1\n"
		<< "uuid=22210203-0405-0607-0809-0a0b0c0d0e0f\n"
		<< "manufacturer=Broadcom\n"
		<< "model_name=SoftAP\n"
		<< "model_number=0\n"
		<< "serial_number=0\n"
		<< "device_type=6-a4c0e1f4-1\n"
		<< "device_name=" << device_name << "\n"
		<< "os_version=80000000\n"
		<< "config_methods=label push_button\n"
		// Match the real Wii U pairing beacon ordering: Broadcom's vendor HT
		// compatibility IE, Nintendo-OUI WPS IE, Broadcom compatibility IE,
		// then WMM (the latter two are placed by the Tendonin hostapd patch).
		<< "vendor_elements=dd0800904c0700513206dd09001018020100040000\n";

	if (ignore_broadcast_probe_requests)
	{
		// Hidden SSID mode: ignore wildcard Probe Requests and only respond to directed SSID probes.
		conf << "ignore_broadcast_ssid=1\n";
	}

	if (!conf.good())
	{
		error = "failed while writing hostapd pairing config";
		return false;
	}
	return true;
}

bool WriteRuntimeHostapdConfig(const std::string& iface, const drc_host::MacAddress& ap_mac, const std::string& runtime_ssid, const std::string& psk_hex, int channel, const std::string& output_path, std::string& error)
{
	if (channel <= 0)
	{
		error = "invalid channel";
		return false;
	}
	if (runtime_ssid.empty() || runtime_ssid.size() > 32)
	{
		error = "invalid runtime ssid";
		return false;
	}
	if (psk_hex.size() != 64)
	{
		error = "invalid runtime psk";
		return false;
	}

	const bool use5ghz = channel > 14;
	const char* hw_mode = use5ghz ? "a" : "g";

	std::ofstream conf(output_path, std::ios::trunc);
	if (!conf)
	{
		error = "failed to open hostapd runtime config";
		return false;
	}

	conf
		<< "interface=" << iface << "\n"
		<< "driver=nl80211\n"
		<< "logger_stdout=-1\n"
		<< "logger_stdout_level=0\n"
		<< "ctrl_interface=" << kHostapdControlPath << "\n"
		<< "bssid=" << ap_mac.to_string() << "\n"
		<< "hw_mode=" << hw_mode << "\n"
		<< "channel=" << channel << "\n"
		<< "beacon_int=100\n"
		<< "dtim_period=3\n"
		<< "macaddr_acl=0\n"
		<< "auth_algs=3\n"
		<< "wmm_enabled=1\n"
		<< "uapsd_advertisement_enabled=1\n"
		<< "ieee80211n=1\n"
		<< "ssid=" << runtime_ssid << "\n"
		<< "ignore_broadcast_ssid=2\n"
		<< "wpa=2\n"
		<< "wpa_psk=" << psk_hex << "\n"
		<< "wpa_key_mgmt=WPA-PSK\n"
		<< "wpa_pairwise=CCMP\n";

	if (!conf.good())
	{
		error = "failed while writing hostapd runtime config";
		return false;
	}
	return true;
}

std::string BuildTempPath(const std::string& tag)
{
	const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
	std::ostringstream out;
	out << "/tmp/drcd-" << tag << "-" << static_cast<long>(::getpid()) << "-" << nonce;
	return out.str();
}

bool AppendDhcpOption(std::vector<uint8_t>& options, uint8_t code, std::initializer_list<uint8_t> payload)
{
	if (payload.size() > 255)
		return false;
	options.push_back(code);
	options.push_back(static_cast<uint8_t>(payload.size()));
	options.insert(options.end(), payload.begin(), payload.end());
	return true;
}

bool AppendDhcpOptionU32(std::vector<uint8_t>& options, uint8_t code, uint32_t value)
{
	value = htonl(value);
	const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
	return AppendDhcpOption(options, code, {bytes[0], bytes[1], bytes[2], bytes[3]});
}

bool AppendDhcpOptionU16(std::vector<uint8_t>& options, uint8_t code, uint16_t value)
{
	value = htons(value);
	const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
	return AppendDhcpOption(options, code, {bytes[0], bytes[1]});
}

std::optional<DhcpMessageType> FindDhcpMessageType(const uint8_t* options, size_t length)
{
	for (size_t i = 0; i < length;)
	{
		const uint8_t code = options[i++];
		if (code == 0)
			continue;
		if (code == 255 || i >= length)
			break;
		const uint8_t option_length = options[i++];
		if (i + option_length > length)
			break;
		if (code == 53 && option_length == 1)
			return static_cast<DhcpMessageType>(options[i]);
		i += option_length;
	}
	return std::nullopt;
}

std::vector<uint8_t> BuildDhcpReply(const DhcpPacket& request, DhcpMessageType response_type)
{
	DhcpPacket reply{};
	reply.op = 2;
	reply.htype = request.htype;
	reply.hlen = request.hlen;
	reply.xid = request.xid;
	reply.secs = request.secs;
	reply.flags = request.flags;
	reply.yiaddr = inet_addr(kGamePadIp.data());
	reply.siaddr = inet_addr(kConsoleIp.data());
	std::copy(std::begin(request.chaddr), std::end(request.chaddr), std::begin(reply.chaddr));

	std::vector<uint8_t> output(kDhcpHeaderSize + sizeof(uint32_t));
	std::memcpy(output.data(), &reply, kDhcpHeaderSize);
	const uint32_t cookie = htonl(kDhcpMagicCookie);
	std::memcpy(output.data() + kDhcpHeaderSize, &cookie, sizeof(cookie));

	std::vector<uint8_t> options;
	AppendDhcpOption(options, 53, {static_cast<uint8_t>(response_type)});
	AppendDhcpOptionU32(options, 54, ntohl(inet_addr(kConsoleIp.data())));
	AppendDhcpOptionU32(options, 51, kDhcpLeaseSeconds);
	AppendDhcpOption(options, 1, {255, 255, 255, 0});
	AppendDhcpOptionU32(options, 3, ntohl(inet_addr(kConsoleIp.data())));
	AppendDhcpOptionU32(options, 28, ntohl(inet_addr(kBroadcastIp.data())));
	AppendDhcpOptionU16(options, 26, kSessionMtu);
	options.push_back(255);
	output.insert(output.end(), options.begin(), options.end());
	return output;
}

bool SetNetworkManagerManaged(const std::string& interface_name, bool managed, std::string& detail)
{
	int pipe_fd[2]{-1, -1};
	if (pipe(pipe_fd) != 0)
	{
		detail = std::strerror(errno);
		return false;
	}
	const pid_t pid = fork();
	if (pid < 0)
	{
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		detail = std::strerror(errno);
		return false;
	}
	if (pid == 0)
	{
		dup2(pipe_fd[1], STDOUT_FILENO);
		dup2(pipe_fd[1], STDERR_FILENO);
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		execlp("nmcli", "nmcli", "device", "set", interface_name.c_str(),
			"managed", managed ? "yes" : "no", static_cast<char*>(nullptr));
		_exit(127);
	}

	close(pipe_fd[1]);
	std::array<char, 512> buffer{};
	for (;;)
	{
		const ssize_t count = read(pipe_fd[0], buffer.data(), buffer.size());
		if (count > 0)
			detail.append(buffer.data(), static_cast<size_t>(count));
		else
			break;
	}
	close(pipe_fd[0]);
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
	{
	}
	detail = Trim(detail);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

class LinuxSessionBackend final : public SessionBackend
{
public:
	LinuxSessionBackend()
		: m_hostapd_binary(ReadEnvOrDefault("DRCD_HOSTAPD_BIN", DRCD_DEFAULT_HOSTAPD_BIN))
		, m_use_pkexec(ParseBoolEnv("DRCD_USE_PKEXEC", false))
		, m_channel(ParseIntEnv("DRCD_AP_CHANNEL", 36))
		, m_verbose_logging(ParseBoolEnv("DRCD_LOG_STDERR", false))
		, m_hostapd_raw_logging(ParseBoolEnv("DRCD_LOG_HOSTAPD_RAW", false))
		, m_log_path(ReadEnvOrDefault("DRCD_LOG_FILE", "/tmp/drcd.log"))
		, m_credentials_path(ReadEnvOrDefault("DRCD_CREDENTIALS_FILE", "/var/lib/drcd/credentials.conf"))
		, m_pair_ignore_broadcast_probe_requests(ParseBoolEnv("DRCD_PAIR_IGNORE_BROADCAST_PROBES", false))
		, m_pair_channel_sweep(ParseBoolEnv("DRCD_PAIR_CHANNEL_SWEEP", true))
		, m_pair_channel_list_override(ReadEnvOrDefault("DRCD_PAIR_CHANNEL_LIST", ""))
		, m_pair_channel_dwell_ms(std::clamp(ParseIntEnv("DRCD_PAIR_CHANNEL_DWELL_MS", 20000), 1000, 60000))
		, m_wps_pin_timeout_seconds(std::clamp(ParseIntEnv("DRCD_WPS_PIN_TIMEOUT", kDefaultWpsPinTimeoutSeconds), 1, kMaxWpsPinTimeoutSeconds))
	{
		if (m_channel <= 0)
			m_channel = 36;
		m_log_stream.open(m_log_path, std::ios::out | std::ios::trunc);
		if (m_log_stream.good())
			m_log_stream << "drcd diagnostic log\n";
	}

	~LinuxSessionBackend() override
	{
		(void)stop_session();
	}

	BackendResult start_pairing(const PairStartRequest& request) override
	{
		m_pairing_complete_requested.store(false);
		if (is_stop_requested())
			return Fail("operation cancelled");
		if (!ValidateInterfaceName(request.interface_name))
			return Fail("invalid interface name");
		if (!request.pairing_code.valid())
			return Fail("invalid pairing code");

		(void)stop_session();

		m_base_interface = request.interface_name;
		m_ap_interface = request.interface_name;
		m_ap_mac = request.ap_mac;
		m_pairing_code = request.pairing_code;
		m_test_media_path = request.test_media_path;
		m_test_black_frames = request.test_black_frames;
		m_runtime_ssid = "WiiU" + m_ap_mac.to_hex_no_separator();
		m_psk_hex = GeneratePskHex();
		m_probe_seen.clear();
		m_targeted_wps_pin_at.clear();
		m_pairing_activity_seen.store(false);
		m_credential_blob_path = BuildTempPath("cred") + ".bin";
		m_pairing_config_path = BuildTempPath("pair") + ".conf";
		Log("pair-start: iface=" + m_ap_interface +
			" ap_mac=" + m_ap_mac.to_string() +
			" code=" + std::to_string(m_pairing_code.numeric()) +
			" hostapd=" + Quote(m_hostapd_binary) +
			" channel=" + std::to_string(m_channel) +
			" pkexec=" + std::string(m_use_pkexec ? "1" : "0") +
			" wpsPinTimeout=" + std::to_string(m_wps_pin_timeout_seconds) +
			" ignoreBroadcastProbes=" + std::string(m_pair_ignore_broadcast_probe_requests ? "1" : "0") +
			" channelSweep=" + std::string(m_pair_channel_sweep ? "1" : "0") +
			" channelDwellMs=" + std::to_string(m_pair_channel_dwell_ms) +
			" channelListOverride=" + Quote(m_pair_channel_list_override));

		std::string error;
		if (!PrepareInterfaceForAp(error))
			return AbortStart("failed to prepare interface for AP mode: " + error);

		if (!WritePairingCredentialsBlob(m_runtime_ssid, m_psk_hex, m_ap_mac, m_credential_blob_path, error))
			return AbortStart("failed to build WPS credential blob: " + error);
		Log("pair-start: wrote credential blob " + m_credential_blob_path);

		const std::string pairing_ssid = drc_host::build_pairing_ssid(m_ap_mac, m_pairing_code);
		const auto channel_plan = BuildPairingChannelPlan(m_channel, m_pair_channel_sweep, m_pair_channel_list_override);
		Log("pair-start: channel plan=" + JoinChannels(channel_plan));
		int selected_channel = -1;
		std::string last_channel_error;
		for (size_t channel_index = 0; channel_index < channel_plan.size(); ++channel_index)
		{
			const int attempt_channel = channel_plan[channel_index];
			Log("pair-start: channel attempt " + std::to_string(channel_index + 1) +
				"/" + std::to_string(channel_plan.size()) +
				" channel=" + std::to_string(attempt_channel));

			if (!WritePairingHostapdConfig(
					m_ap_interface,
					m_ap_mac,
					pairing_ssid,
					m_credential_blob_path,
					attempt_channel,
					m_pair_ignore_broadcast_probe_requests,
					m_pairing_config_path,
					error))
			{
				last_channel_error = "failed to build hostapd pairing config: " + error;
				break;
			}
			Log("pair-start: wrote hostapd pairing config " + m_pairing_config_path +
				" ssid=" + pairing_ssid +
				" channel=" + std::to_string(attempt_channel));

			if (!StartHostapd(m_pairing_config_path, error))
			{
				last_channel_error = "failed to launch hostapd (" + Quote(m_hostapd_binary) + "): " + error;
				break;
			}
			Log("pair-start: hostapd started, waiting for AP-ENABLED");

			if (WaitForHostapdReady("pairing", std::chrono::seconds(20), error))
			{
				selected_channel = attempt_channel;
				Log("pair-start: AP became ready on channel " + std::to_string(selected_channel));
				break;
			}

			last_channel_error = error;
			Log("pair-start: channel " + std::to_string(attempt_channel) + " failed: " + error);
			StopHostapd();
		}

		if (selected_channel <= 0)
		{
			if (last_channel_error.empty())
				last_channel_error = "unknown channel selection failure";
			return AbortStart("all pairing channel attempts failed: " + last_channel_error);
		}
		m_channel = selected_channel;
		Log("pair-start: AP-ENABLED observed, arming WPS_PIN");

		if (!ArmWpsPinAny(error))
			return AbortStart("failed to arm WPS pin: " + error);
		Log("pair-start: WPS_PIN armed successfully");
		m_pairing_activity_seen.store(false);
		StartHostapdMonitor("pairing");

		m_snapshot.phase = "pairing";
		m_snapshot.base_interface = m_base_interface;
		m_snapshot.ap_interface = m_ap_interface;
		m_snapshot.using_virtual_ap = false;
		m_snapshot.last_error.clear();
		StartPairChannelSweep(channel_plan, pairing_ssid);
		return {true, "pairing AP ready"};
	}

	BackendResult enter_runtime() override
	{
		if (is_stop_requested())
			return Fail("operation cancelled");
		if (m_snapshot.phase != "pairing")
			return Fail("backend is not in pairing phase");

		if (!SaveCredentials())
			Log("runtime-start: warning: could not persist credentials to " + m_credentials_path);
		return StartRuntimeAp(true);
	}

	BackendResult start_runtime(const PairStartRequest& request) override
	{
		if (is_stop_requested())
			return Fail("operation cancelled");
		if (!ValidateInterfaceName(request.interface_name))
			return Fail("invalid interface name");
		(void)stop_session();
		m_base_interface = request.interface_name;
		m_ap_interface = request.interface_name;
		m_ap_mac = request.ap_mac;
		m_pairing_code = request.pairing_code;
		m_test_media_path = request.test_media_path;
		m_test_black_frames = request.test_black_frames;
		if (!LoadCredentials())
			return AbortStart("no saved credentials for AP " + request.ap_mac.to_string());
		std::string error;
		if (!PrepareInterfaceForAp(error))
			return AbortStart("failed to prepare interface for runtime AP mode: " + error);
		m_snapshot.phase = "pairing";
		m_snapshot.base_interface = m_base_interface;
		m_snapshot.ap_interface = m_ap_interface;
		return StartRuntimeAp(false);
	}

private:
	BackendResult StartRuntimeAp(bool from_pairing)
	{
		StopPairChannelSweep();
		std::string error;
		if (from_pairing)
		{
			Log("runtime-start: stopping pairing hostapd");
			StopHostapd();
			if (!ResetInterfaceForApRestart(error))
				return Fail("failed to reset interface for runtime AP: " + error);
			std::this_thread::sleep_for(std::chrono::milliseconds(750));
		}
		m_runtime_config_path = BuildTempPath("runtime") + ".conf";
		if (!WriteRuntimeHostapdConfig(m_ap_interface, m_ap_mac, m_runtime_ssid, m_psk_hex, m_channel, m_runtime_config_path, error))
			return Fail("failed to build runtime hostapd config: " + error);
		Log("runtime-start: wrote hostapd runtime config " + m_runtime_config_path);

		bool runtime_ready = false;
		for (int attempt = 1; attempt <= 3; ++attempt)
		{
			Log("runtime-start: AP attempt " + std::to_string(attempt) + "/3");
			if (!StartHostapd(m_runtime_config_path, error))
				return Fail("failed to launch runtime hostapd (" + Quote(m_hostapd_binary) + "): " + error);
			Log("runtime-start: hostapd started, waiting for AP-ENABLED");
			if (WaitForHostapdReady("runtime", std::chrono::seconds(20), error))
			{
				runtime_ready = true;
				break;
			}

			Log("runtime-start: AP attempt " + std::to_string(attempt) + " failed: " + error);
			StopHostapd();
			if (attempt < 3)
			{
				std::string reset_error;
				if (!ResetInterfaceForApRestart(reset_error))
					return Fail("failed to reset interface before runtime retry: " + reset_error);
				std::this_thread::sleep_for(std::chrono::milliseconds(750 * attempt));
			}
		}
		if (!runtime_ready)
			return Fail("runtime AP failed after 3 attempts: " + error);
		Log("runtime-start: AP-ENABLED observed");
		if (!StartRuntimeNetwork(error))
		{
			StopHostapd();
			return Fail("failed to start runtime network: " + error);
		}
		ConfigureRuntimeMediaQos();
		const bool tsf_monitor_ready = StartTsfMonitor();
		m_runtime_transport = std::make_unique<drc_host::RuntimeTransport>();
		if (!m_runtime_transport->start({
				.interface_name = m_ap_interface,
				.tsf_monitor_interface = tsf_monitor_ready ? std::string(kTsfMonitorInterface) : std::string{},
				.console_address = std::string(kConsoleIp),
				.gamepad_address = std::string(kGamePadIp),
			}, error))
		{
			m_runtime_transport.reset();
			StopTsfMonitor();
			StopDhcpServer();
			RestoreRuntimeMediaQos();
			StopHostapd();
			return Fail("failed to start DRC protocol transport: " + error);
		}
		Log("runtime-protocol: transport started");
		const char* app_hook_socket = std::getenv("BARISTA_MUG_SOCKET");
		if (!m_test_media_path.empty() || m_test_black_frames || (app_hook_socket && *app_hook_socket))
		{
			m_media_streamer = std::make_unique<MediaStreamer>(*m_runtime_transport,
				m_test_media_path, m_test_black_frames);
			QueueStatus("Test media ready; playback will start when the GamePad protocol connects");
		}
		StartHostapdMonitor("runtime");
		QueueStatus("Runtime AP ready on channel " + std::to_string(m_channel) +
			"; DHCP waiting at " + std::string(kConsoleIp));

		m_snapshot.phase = "runtime";
		m_snapshot.last_error.clear();
		return {true, "runtime AP ready"};
	}

public:

	BackendResult stop_session() override
	{
		Log("stop-session: stopping hostapd and cleaning temporary files");
		StopPairChannelSweep();
		if (m_media_streamer)
		{
			m_media_streamer->stop();
			m_media_streamer.reset();
		}
		if (m_runtime_transport)
		{
			m_runtime_transport->stop();
			m_runtime_transport.reset();
		}
		StopTsfMonitor();
		StopDhcpServer();
		RestoreRuntimeMediaQos();
		StopHostapd();
		RestoreInterface();
		CleanupTemporaryFiles();
		m_probe_seen.clear();
		m_targeted_wps_pin_at.clear();
		m_pairing_complete_requested.store(false);
		m_gamepad_disconnected_requested.store(false);
		m_base_interface.clear();
		m_ap_interface.clear();
		m_runtime_ssid.clear();
		m_psk_hex.clear();
		m_snapshot = {};
		return {true, "session stopped"};
	}

	BackendSnapshot snapshot() const override
	{
		BackendSnapshot snapshot = m_snapshot;
		if (m_runtime_transport)
		{
			const auto stats = m_runtime_transport->stats();
			snapshot.battery_charge_valid = stats.battery_charge_valid;
			snapshot.battery_charge = stats.battery_charge;
		}
		return snapshot;
	}

	bool consume_pairing_complete_event() override
	{
		return m_pairing_complete_requested.exchange(false);
	}

	bool consume_gamepad_connected_event() override
	{
		if (!m_runtime_transport || !m_runtime_transport->consume_ready_event())
			return false;
		if (m_media_streamer && !m_media_streamer->running())
		{
			std::string error;
			if (m_media_streamer->start(error))
			{
				const std::string source = std::getenv("BARISTA_MUG_SOCKET")
					? "MUG AppHook" : (m_test_black_frames
					? "built-in generated frames" : m_test_media_path);
				Log("media: streaming " + source);
				QueueStatus("Streaming media to GamePad: " + source);
			}
			else
			{
				Log("media: failed to start: " + error);
				QueueStatus("Test media failed: " + error);
			}
		}
		return true;
	}

	bool consume_gamepad_disconnected_event() override
	{
		const bool transport_event = m_runtime_transport && m_runtime_transport->consume_disconnected_event();
		const bool hostapd_event = m_gamepad_disconnected_requested.exchange(false);
		if ((transport_event || hostapd_event) && m_media_streamer && m_media_streamer->running())
		{
			m_media_streamer->stop();
			Log("media: stopped after GamePad disconnect; encoder will reinitialize on reconnect");
			QueueStatus("Test media paused until the GamePad and UVC/UAC keepalive reconnect");
		}
		return transport_event || hostapd_event;
	}

	std::optional<std::string> consume_status_event() override
	{
		if (m_runtime_transport)
		{
			if (auto event = m_runtime_transport->consume_status_event())
			{
				Log("runtime-protocol: " + *event);
				return event;
			}
		}
		std::lock_guard lock(m_status_mutex);
		if (m_status_events.empty())
			return std::nullopt;
		std::string event = std::move(m_status_events.front());
		m_status_events.pop_front();
		return event;
	}

private:
	void ConfigureRuntimeMediaQos()
	{
		m_per_tid_rts_enabled = false;
		m_global_rts_enabled = false;
		m_fixed_runtime_bitrate_enabled = false;
		m_runtime_phy_name.clear();

		std::string output;
		if (RunIw({"dev", m_ap_interface, "set", "tidconf", "tids", "0x20",
				"rtscts", "on"}, output))
		{
			m_per_tid_rts_enabled = true;
			Log("runtime-qos: enabled RTS/CTS for media TID 5");
		}
		else
		{
			Log("runtime-qos: could not enable RTS/CTS for media TID 5" +
				(output.empty() ? std::string{} : ": " + output));

			std::error_code filesystem_error;
			const auto phy_path = std::filesystem::canonical(
				std::filesystem::path("/sys/class/net") / m_ap_interface / "phy80211",
				filesystem_error);
			if (filesystem_error || phy_path.filename().empty())
			{
				Log("runtime-qos: could not resolve wireless PHY for global RTS fallback");
			}
			else
			{
				m_runtime_phy_name = phy_path.filename().string();
				output.clear();
				if (RunIw({"phy", m_runtime_phy_name, "set", "rts", "0"}, output))
				{
					m_global_rts_enabled = true;
					Log("runtime-qos: enabled global RTS fallback on " + m_runtime_phy_name);
				}
				else
				{
					Log("runtime-qos: could not enable global RTS fallback on " +
						m_runtime_phy_name +
						(output.empty() ? std::string{} : ": " + output));
					m_runtime_phy_name.clear();
				}
			}
		}

		output.clear();
		if (RunIw({"dev", m_ap_interface, "set", "bitrates", "ht-mcs-5", "6",
				"lgi-5"}, output))
		{
			m_fixed_runtime_bitrate_enabled = true;
			Log("runtime-qos: fixed runtime TX rate to HT20 MCS 6 long GI");
		}
		else
		{
			Log("runtime-qos: could not fix runtime TX rate to HT20 MCS 6" +
				(output.empty() ? std::string{} : ": " + output));
		}
	}

	void RestoreRuntimeMediaQos()
	{
		std::string output;
		if (m_per_tid_rts_enabled)
		{
			if (!RunIw({"dev", m_ap_interface, "set", "tidconf", "tids", "0x20",
					"rtscts", "off"}, output))
			{
				Log("runtime-qos: could not disable media TID 5 RTS/CTS" +
					(output.empty() ? std::string{} : ": " + output));
			}
		}
		if (m_global_rts_enabled && !m_runtime_phy_name.empty())
		{
			output.clear();
			if (RunIw({"phy", m_runtime_phy_name, "set", "rts", "off"}, output))
				Log("runtime-qos: restored default RTS behavior on " + m_runtime_phy_name);
			else
				Log("runtime-qos: could not restore default RTS behavior on " +
					m_runtime_phy_name + (output.empty() ? std::string{} : ": " + output));
		}
		if (m_fixed_runtime_bitrate_enabled)
		{
			output.clear();
			if (RunIw({"dev", m_ap_interface, "set", "bitrates"}, output))
				Log("runtime-qos: restored automatic runtime TX rate selection");
			else
				Log("runtime-qos: could not restore automatic runtime TX rate selection" +
					(output.empty() ? std::string{} : ": " + output));
		}
		m_per_tid_rts_enabled = false;
		m_global_rts_enabled = false;
		m_fixed_runtime_bitrate_enabled = false;
		m_runtime_phy_name.clear();
	}

	bool RunIw(const std::vector<std::string>& arguments, std::string& output) const
	{
		int pipe_fd[2]{-1, -1};
		if (pipe(pipe_fd) != 0)
		{
			output = std::strerror(errno);
			return false;
		}
		const pid_t pid = fork();
		if (pid < 0)
		{
			close(pipe_fd[0]);
			close(pipe_fd[1]);
			output = std::strerror(errno);
			return false;
		}
		if (pid == 0)
		{
			dup2(pipe_fd[1], STDOUT_FILENO);
			dup2(pipe_fd[1], STDERR_FILENO);
			close(pipe_fd[0]);
			close(pipe_fd[1]);
			std::vector<char*> argv;
			argv.reserve(arguments.size() + 2);
			argv.push_back(const_cast<char*>("iw"));
			for (const auto& argument : arguments)
				argv.push_back(const_cast<char*>(argument.c_str()));
			argv.push_back(nullptr);
			execvp("iw", argv.data());
			_exit(127);
		}

		close(pipe_fd[1]);
		std::array<char, 512> buffer{};
		for (;;)
		{
			const ssize_t count = read(pipe_fd[0], buffer.data(), buffer.size());
			if (count > 0)
				output.append(buffer.data(), static_cast<size_t>(count));
			else if (count == 0)
				break;
			else if (errno != EINTR)
				break;
		}
		close(pipe_fd[0]);
		int status = 0;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		{
		}
		output = Trim(output);
		return WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}

	bool StartTsfMonitor()
	{
		StopTsfMonitor();
		std::string output;
		if (!RunIw({"dev", m_ap_interface, "interface", "add", std::string(kTsfMonitorInterface),
				"type", "monitor", "flags", "control", "otherbss"}, output))
		{
			Log("tsf-monitor: could not create " + std::string(kTsfMonitorInterface) +
				(output.empty() ? std::string{} : ": " + output));
			QueueStatus("Hardware TSF monitor unavailable; media clock will use fallback");
			return false;
		}

		const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
		{
			Log("tsf-monitor: socket failed: " + std::string(std::strerror(errno)));
			StopTsfMonitor();
			return false;
		}
		ifreq flags{};
		std::snprintf(flags.ifr_name, sizeof(flags.ifr_name), "%s", kTsfMonitorInterface.data());
		bool ready = ioctl(fd, SIOCGIFFLAGS, &flags) == 0;
		if (ready)
		{
			flags.ifr_flags = static_cast<short>(flags.ifr_flags | IFF_UP);
			ready = ioctl(fd, SIOCSIFFLAGS, &flags) == 0;
		}
		const std::string error = ready ? std::string{} : std::strerror(errno);
		close(fd);
		if (!ready)
		{
			Log("tsf-monitor: could not bring interface up: " + error);
			StopTsfMonitor();
			return false;
		}
		m_tsf_monitor_created = true;
		Log("tsf-monitor: " + std::string(kTsfMonitorInterface) +
			" active on the GamePad PHY");
		return true;
	}

	void StopTsfMonitor()
	{
		std::string output;
		const bool removed = RunIw({"dev", std::string(kTsfMonitorInterface), "del"}, output);
		if (m_tsf_monitor_created && !removed)
			Log("tsf-monitor: could not remove " + std::string(kTsfMonitorInterface) +
				(output.empty() ? std::string{} : ": " + output));
		m_tsf_monitor_created = false;
	}

	void QueueStatus(std::string message)
	{
		std::lock_guard lock(m_status_mutex);
		m_status_events.emplace_back(std::move(message));
	}

	bool ConfigureRuntimeInterface(std::string& error)
	{
		const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
		{
			error = "socket(): " + std::string(std::strerror(errno));
			return false;
		}
		auto set_address = [&](unsigned long request, std::string_view address, const char* label) {
			ifreq ifr{};
			std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", m_ap_interface.c_str());
			auto* value = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr);
			value->sin_family = AF_INET;
			if (inet_pton(AF_INET, address.data(), &value->sin_addr) != 1 || ioctl(fd, request, &ifr) != 0)
			{
				error = std::string("could not set ") + label + ": " + std::strerror(errno);
				return false;
			}
			return true;
		};
		if (!set_address(SIOCSIFADDR, kConsoleIp, "address") ||
			!set_address(SIOCSIFNETMASK, "255.255.255.0", "netmask"))
		{
			close(fd);
			return false;
		}
		ifreq mtu{};
		std::snprintf(mtu.ifr_name, sizeof(mtu.ifr_name), "%s", m_ap_interface.c_str());
		mtu.ifr_mtu = kSessionMtu;
		if (ioctl(fd, SIOCSIFMTU, &mtu) != 0)
		{
			error = "could not set MTU: " + std::string(std::strerror(errno));
			close(fd);
			return false;
		}
		close(fd);
		Log("runtime-network: configured " + m_ap_interface + " " +
			std::string(kConsoleIp) + "/24 mtu=" + std::to_string(kSessionMtu));
		return true;
	}

	bool StartRuntimeNetwork(std::string& error)
	{
		StopDhcpServer();
		if (!ConfigureRuntimeInterface(error))
			return false;

		m_dhcp_socket = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
		if (m_dhcp_socket < 0)
		{
			error = "DHCP socket(): " + std::string(std::strerror(errno));
			return false;
		}
		const int one = 1;
		(void)setsockopt(m_dhcp_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		(void)setsockopt(m_dhcp_socket, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
#ifdef SO_BINDTODEVICE
		if (setsockopt(m_dhcp_socket, SOL_SOCKET, SO_BINDTODEVICE,
			m_ap_interface.c_str(), m_ap_interface.size() + 1) != 0)
		{
			error = "DHCP SO_BINDTODEVICE: " + std::string(std::strerror(errno));
			close(m_dhcp_socket);
			m_dhcp_socket = -1;
			return false;
		}
#endif
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(67);
		// DHCP clients do not have an address yet and normally transmit to the
		// limited broadcast address. A socket bound only to 192.168.1.10 does
		// not receive those datagrams; SO_BINDTODEVICE still limits this
		// wildcard bind to the GamePad interface.
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(m_dhcp_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		{
			error = "DHCP bind(*:67): " + std::string(std::strerror(errno));
			close(m_dhcp_socket);
			m_dhcp_socket = -1;
			return false;
		}

		m_dhcp_stop.store(false);
		m_dhcp_thread = std::thread([this]() { DhcpServerLoop(); });
		Log("dhcp: serving " + std::string(kGamePadIp) + " on " + m_ap_interface);
		return true;
	}

	void StopDhcpServer()
	{
		m_dhcp_stop.store(true);
		if (m_dhcp_thread.joinable())
			m_dhcp_thread.join();
		if (m_dhcp_socket >= 0)
		{
			close(m_dhcp_socket);
			m_dhcp_socket = -1;
		}
	}

	void DhcpServerLoop()
	{
		std::array<uint8_t, 2048> buffer{};
		while (!m_dhcp_stop.load() && !is_stop_requested())
		{
			fd_set read_set;
			FD_ZERO(&read_set);
			FD_SET(m_dhcp_socket, &read_set);
			timeval timeout{.tv_sec = 0, .tv_usec = 250000};
			const int ready = select(m_dhcp_socket + 1, &read_set, nullptr, nullptr, &timeout);
			if (ready <= 0)
				continue;
			sockaddr_in source{};
			socklen_t source_length = sizeof(source);
			const ssize_t size = recvfrom(m_dhcp_socket, buffer.data(), buffer.size(), 0,
				reinterpret_cast<sockaddr*>(&source), &source_length);
			if (size < static_cast<ssize_t>(kDhcpHeaderSize + sizeof(uint32_t)))
				continue;
			const auto* request = reinterpret_cast<const DhcpPacket*>(buffer.data());
			if (request->op != 1 || request->htype != 1 || request->hlen != 6)
				continue;
			uint32_t cookie = 0;
			std::memcpy(&cookie, buffer.data() + kDhcpHeaderSize, sizeof(cookie));
			if (ntohl(cookie) != kDhcpMagicCookie)
				continue;
			const auto type = FindDhcpMessageType(buffer.data() + kDhcpHeaderSize + sizeof(cookie),
				static_cast<size_t>(size) - kDhcpHeaderSize - sizeof(cookie));
			DhcpMessageType response_type{};
			if (type == DhcpMessageType::Discover)
				response_type = DhcpMessageType::Offer;
			else if (type == DhcpMessageType::Request)
				response_type = DhcpMessageType::Ack;
			else
				continue;

			const auto response = BuildDhcpReply(*request, response_type);
			sockaddr_in destination{};
			destination.sin_family = AF_INET;
			destination.sin_port = htons(68);
			inet_pton(AF_INET, kBroadcastIp.data(), &destination.sin_addr);
			const ssize_t sent = sendto(m_dhcp_socket, response.data(), response.size(), 0,
				reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
			if (sent < 0)
			{
				Log("dhcp: reply failed: " + std::string(std::strerror(errno)));
				QueueStatus("DHCP reply failed: " + std::string(std::strerror(errno)));
				continue;
			}
			if (response_type == DhcpMessageType::Offer)
				QueueStatus("GamePad requested an address; offered " + std::string(kGamePadIp));
			else
				QueueStatus("GamePad DHCP lease active at " + std::string(kGamePadIp));
			Log(std::string("dhcp: sent ") +
				(response_type == DhcpMessageType::Offer ? "OFFER" : "ACK") +
				" for " + std::string(kGamePadIp));
		}
	}

	struct SavedInterfaceState
	{
		bool valid = false;
		bool was_up = false;
		int mtu = 1500;
		std::array<uint8_t, 6> mac{};
	};

	bool SaveCredentials()
	{
		std::error_code ec;
		const std::filesystem::path path(m_credentials_path);
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), ec);
		if (ec)
			return false;
		std::ofstream output(path, std::ios::out | std::ios::trunc);
		if (!output.good())
			return false;
		output << "ap_mac=" << m_ap_mac.to_string() << '\n'
			<< "ssid=" << m_runtime_ssid << '\n'
			<< "psk=" << m_psk_hex << '\n'
			<< "channel=" << m_channel << '\n';
		output.close();
		(void)::chmod(path.c_str(), static_cast<mode_t>(0600));
		Log("credentials: saved " + m_credentials_path);
		return output.good();
	}

	bool LoadCredentials()
	{
		std::ifstream input(m_credentials_path);
		if (!input.good())
			return false;
		std::string saved_mac;
		std::string saved_ssid;
		std::string saved_psk;
		int saved_channel = 0;
		std::string line;
		while (std::getline(input, line))
		{
			const size_t separator = line.find('=');
			if (separator == std::string::npos)
				continue;
			const std::string key = line.substr(0, separator);
			const std::string value = line.substr(separator + 1);
			if (key == "ap_mac") saved_mac = value;
			else if (key == "ssid") saved_ssid = value;
			else if (key == "psk") saved_psk = value;
			else if (key == "channel")
			{
				try { saved_channel = std::stoi(value); } catch (...) { return false; }
			}
		}
		const auto parsed_mac = drc_host::MacAddress::parse(saved_mac);
		if (!parsed_mac.has_value() || *parsed_mac != m_ap_mac ||
			saved_ssid != "WiiU" + m_ap_mac.to_hex_no_separator() ||
			saved_psk.size() != 64 || saved_channel <= 0)
			return false;
		m_runtime_ssid = std::move(saved_ssid);
		m_psk_hex = std::move(saved_psk);
		m_channel = saved_channel;
		Log("credentials: loaded " + m_credentials_path);
		return true;
	}

	bool SetInterfaceManaged(std::string& error) const
	{
		int pipe_fd[2]{-1, -1};
		if (pipe(pipe_fd) != 0)
		{
			error = "pipe() failed while starting iw: " + std::string(std::strerror(errno));
			return false;
		}

		const pid_t pid = fork();
		if (pid < 0)
		{
			close(pipe_fd[0]);
			close(pipe_fd[1]);
			error = "fork() failed while starting iw: " + std::string(std::strerror(errno));
			return false;
		}
		if (pid == 0)
		{
			dup2(pipe_fd[1], STDOUT_FILENO);
			dup2(pipe_fd[1], STDERR_FILENO);
			close(pipe_fd[0]);
			close(pipe_fd[1]);
			execlp("iw", "iw", "dev", m_ap_interface.c_str(), "set", "type", "managed",
				static_cast<char*>(nullptr));
			dprintf(STDERR_FILENO, "exec iw failed: %s\n", std::strerror(errno));
			_exit(127);
		}

		close(pipe_fd[1]);
		std::string output;
		std::array<char, 512> buffer{};
		for (;;)
		{
			const ssize_t count = read(pipe_fd[0], buffer.data(), buffer.size());
			if (count > 0)
				output.append(buffer.data(), static_cast<size_t>(count));
			else if (count == 0)
				break;
			else if (errno != EINTR)
				break;
		}
		close(pipe_fd[0]);

		int status = 0;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		{
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		{
			error = "could not set interface type to managed";
			const std::string detail = Trim(output);
			if (!detail.empty())
				error += ": " + detail;
			return false;
		}
		return true;
	}

	bool PrepareInterfaceForAp(std::string& error)
	{
		const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
		{
			error = "socket() failed: " + std::string(std::strerror(errno));
			return false;
		}

		ifreq ifr{};
		std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", m_ap_interface.c_str());
		if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0)
		{
			error = "could not read interface flags: " + std::string(std::strerror(errno));
			close(fd);
			return false;
		}
		const short original_flags = ifr.ifr_flags;

		ifreq hwaddr{};
		std::snprintf(hwaddr.ifr_name, sizeof(hwaddr.ifr_name), "%s", m_ap_interface.c_str());
		if (ioctl(fd, SIOCGIFHWADDR, &hwaddr) != 0)
		{
			error = "could not read interface MAC: " + std::string(std::strerror(errno));
			close(fd);
			return false;
		}

		m_saved_interface.valid = true;
		m_saved_interface.was_up = (original_flags & IFF_UP) != 0;
		std::memcpy(m_saved_interface.mac.data(), hwaddr.ifr_hwaddr.sa_data,
			m_saved_interface.mac.size());
		ifreq mtu{};
		std::snprintf(mtu.ifr_name, sizeof(mtu.ifr_name), "%s", m_ap_interface.c_str());
		if (ioctl(fd, SIOCGIFMTU, &mtu) == 0)
			m_saved_interface.mtu = mtu.ifr_mtu;

		std::string nm_detail;
		if (SetNetworkManagerManaged(m_ap_interface, false, nm_detail))
		{
			m_network_manager_claimed = true;
			Log("interface-prep: NetworkManager released " + m_ap_interface);
			QueueStatus("Wi-Fi interface reserved for GamePad hosting");
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
		else
		{
			Log("interface-prep: NetworkManager release unavailable" +
				(nm_detail.empty() ? std::string{} : ": " + nm_detail));
		}

		if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0)
		{
			error = "could not refresh interface flags: " + std::string(std::strerror(errno));
			close(fd);
			return false;
		}
		if ((ifr.ifr_flags & IFF_UP) != 0)
		{
			ifr.ifr_flags = static_cast<short>(original_flags & ~IFF_UP);
			if (ioctl(fd, SIOCSIFFLAGS, &ifr) != 0)
			{
				error = "could not bring interface down: " + std::string(std::strerror(errno));
				close(fd);
				return false;
			}
		}

		if (!SetInterfaceManaged(error))
		{
			close(fd);
			return false;
		}

		hwaddr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
		std::memcpy(hwaddr.ifr_hwaddr.sa_data, m_ap_mac.bytes.data(), m_ap_mac.bytes.size());
		if (ioctl(fd, SIOCSIFHWADDR, &hwaddr) != 0)
		{
			error = "could not set interface MAC to " + m_ap_mac.to_string() + ": " +
				std::string(std::strerror(errno));
			close(fd);
			return false;
		}

		close(fd);
		Log("interface-prep: set " + m_ap_interface + " down with MAC " + m_ap_mac.to_string());
		return true;
	}

	bool ResetInterfaceForApRestart(std::string& error)
	{
		const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
		{
			error = "socket() failed: " + std::string(std::strerror(errno));
			return false;
		}

		ifreq flags{};
		std::snprintf(flags.ifr_name, sizeof(flags.ifr_name), "%s", m_ap_interface.c_str());
		if (ioctl(fd, SIOCGIFFLAGS, &flags) != 0)
		{
			error = "could not read interface flags: " + std::string(std::strerror(errno));
			close(fd);
			return false;
		}
		flags.ifr_flags = static_cast<short>(flags.ifr_flags & ~IFF_UP);
		if (ioctl(fd, SIOCSIFFLAGS, &flags) != 0)
		{
			error = "could not bring interface down: " + std::string(std::strerror(errno));
			close(fd);
			return false;
		}

		if (!SetInterfaceManaged(error))
		{
			close(fd);
			return false;
		}

		ifreq hwaddr{};
		std::snprintf(hwaddr.ifr_name, sizeof(hwaddr.ifr_name), "%s", m_ap_interface.c_str());
		hwaddr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
		std::memcpy(hwaddr.ifr_hwaddr.sa_data, m_ap_mac.bytes.data(), m_ap_mac.bytes.size());
		if (ioctl(fd, SIOCSIFHWADDR, &hwaddr) != 0)
		{
			error = "could not reapply AP MAC: " + std::string(std::strerror(errno));
			close(fd);
			return false;
		}

		close(fd);
		Log("interface-reset: prepared " + m_ap_interface + " for AP restart");
		return true;
	}

	void RestoreInterface()
	{
		if (!m_saved_interface.valid || m_base_interface.empty())
			return;

		const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
		{
			Log("interface-restore: socket() failed: " + std::string(std::strerror(errno)));
			m_saved_interface = {};
			return;
		}

		ifreq ifr{};
		std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", m_base_interface.c_str());
		if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0)
		{
			ifr.ifr_flags = static_cast<short>(ifr.ifr_flags & ~IFF_UP);
			if (ioctl(fd, SIOCSIFFLAGS, &ifr) != 0)
				Log("interface-restore: could not bring " + m_base_interface + " down: " + std::strerror(errno));
		}

		std::string type_error;
		if (!SetInterfaceManaged(type_error))
			Log("interface-restore: " + type_error);

		ifreq hwaddr{};
		std::snprintf(hwaddr.ifr_name, sizeof(hwaddr.ifr_name), "%s", m_base_interface.c_str());
		hwaddr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
		std::memcpy(hwaddr.ifr_hwaddr.sa_data, m_saved_interface.mac.data(), m_saved_interface.mac.size());
		if (ioctl(fd, SIOCSIFHWADDR, &hwaddr) != 0)
			Log("interface-restore: could not restore MAC on " + m_base_interface + ": " + std::strerror(errno));

		ifreq mtu{};
		std::snprintf(mtu.ifr_name, sizeof(mtu.ifr_name), "%s", m_base_interface.c_str());
		mtu.ifr_mtu = m_saved_interface.mtu;
		if (ioctl(fd, SIOCSIFMTU, &mtu) != 0)
			Log("interface-restore: could not restore MTU on " + m_base_interface + ": " + std::strerror(errno));

		if (m_saved_interface.was_up)
		{
			ifreq flags{};
			std::snprintf(flags.ifr_name, sizeof(flags.ifr_name), "%s", m_base_interface.c_str());
			if (ioctl(fd, SIOCGIFFLAGS, &flags) == 0)
			{
				flags.ifr_flags = static_cast<short>(flags.ifr_flags | IFF_UP);
				if (ioctl(fd, SIOCSIFFLAGS, &flags) != 0)
					Log("interface-restore: could not bring " + m_base_interface + " up: " + std::strerror(errno));
			}
		}

		close(fd);
		if (m_network_manager_claimed)
		{
			std::string nm_detail;
			if (!SetNetworkManagerManaged(m_base_interface, true, nm_detail))
				Log("interface-restore: NetworkManager reclaim failed" +
					(nm_detail.empty() ? std::string{} : ": " + nm_detail));
			else
				Log("interface-restore: returned " + m_base_interface + " to NetworkManager");
			m_network_manager_claimed = false;
		}
		Log("interface-restore: restored original MAC and link state on " + m_base_interface);
		m_saved_interface = {};
	}

	BackendResult Fail(const std::string& message)
	{
		Log("failure: " + message);
		m_snapshot.last_error = message;
		return {false, message};
	}

	BackendResult AbortStart(const std::string& message)
	{
		Log("abort-start: " + message);
		StopPairChannelSweep();
		StopHostapd();
		RestoreInterface();
		if (!ParseBoolEnv("DRCD_KEEP_FAILED_TEMP", false))
			CleanupTemporaryFiles();
		m_base_interface.clear();
		m_ap_interface.clear();
		m_runtime_ssid.clear();
		m_psk_hex.clear();
		m_targeted_wps_pin_at.clear();
		m_snapshot.phase = "idle";
		m_snapshot.base_interface.clear();
		m_snapshot.ap_interface.clear();
		m_snapshot.using_virtual_ap = false;
		m_snapshot.last_error = message;
		return {false, message};
	}

	bool StartHostapd(const std::string& config_path, std::string& error)
	{
		if (m_hostapd_pid > 0 || m_hostapd_output_fd != -1)
			StopHostapd();

		int pipe_fd[2]{-1, -1};
		if (pipe(pipe_fd) != 0)
		{
			error = "pipe() failed";
			return false;
		}

		const auto pid = fork();
		if (pid < 0)
		{
			close(pipe_fd[0]);
			close(pipe_fd[1]);
			error = "fork() failed";
			return false;
		}

		if (pid == 0)
		{
			dup2(pipe_fd[1], STDOUT_FILENO);
			dup2(pipe_fd[1], STDERR_FILENO);
			close(pipe_fd[0]);
			close(pipe_fd[1]);

			if (m_use_pkexec)
			{
				execlp("pkexec", "pkexec", m_hostapd_binary.c_str(), "-dd", config_path.c_str(), static_cast<char*>(nullptr));
			}
			else
			{
				execlp(m_hostapd_binary.c_str(), m_hostapd_binary.c_str(), "-dd", config_path.c_str(), static_cast<char*>(nullptr));
			}
			const int exec_errno = errno;
			dprintf(STDERR_FILENO, "drcd: exec failed for hostapd path %s: %s\n",
				m_hostapd_binary.c_str(), std::strerror(exec_errno));
			_exit(127);
		}

		close(pipe_fd[1]);
		m_hostapd_pid = pid;
		m_hostapd_output_fd = pipe_fd[0];
		m_hostapd_log.clear();
		Log("hostapd: pid=" + std::to_string(m_hostapd_pid) + " config=" + config_path);

		const int flags = fcntl(m_hostapd_output_fd, F_GETFL, 0);
		if (flags >= 0)
			(void)fcntl(m_hostapd_output_fd, F_SETFL, flags | O_NONBLOCK);
		return true;
	}

	void StopHostapd()
	{
		StopHostapdMonitor();

		if (m_hostapd_pid > 0)
		{
			const pid_t pid = m_hostapd_pid;
			int status = 0;
			auto wait_res = waitpid(pid, &status, WNOHANG);
			if (wait_res == 0)
			{
				Log("hostapd: sending SIGTERM to pid=" + std::to_string(pid));
				(void)kill(pid, SIGTERM);
				for (int i = 0; i < 20; ++i)
				{
					wait_res = waitpid(pid, &status, WNOHANG);
					if (wait_res == pid || (wait_res < 0 && errno == ECHILD))
						break;
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}

				if (wait_res == 0)
				{
					Log("hostapd: SIGTERM timeout, sending SIGKILL to pid=" + std::to_string(pid));
					(void)kill(pid, SIGKILL);
					while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
					{
					}
				}
			}
			Log("hostapd: stopped");
			m_hostapd_pid = -1;
		}

		if (m_hostapd_output_fd != -1)
		{
			close(m_hostapd_output_fd);
			m_hostapd_output_fd = -1;
		}
	}

	void StartHostapdMonitor(std::string phase)
	{
		StopHostapdMonitor();
		if (m_hostapd_output_fd < 0)
			return;
		m_hostapd_monitor_phase = std::move(phase);
		m_hostapd_monitor_stop.store(false);
		m_hostapd_monitor_thread = std::thread([this]() {
			MonitorHostapdOutputLoop();
		});
		Log("hostapd monitor: started for phase=" + m_hostapd_monitor_phase);
	}

	void StopHostapdMonitor()
	{
		m_hostapd_monitor_stop.store(true);
		if (m_hostapd_monitor_thread.joinable())
			m_hostapd_monitor_thread.join();
		m_hostapd_monitor_phase.clear();
	}

	void StartPairChannelSweep(std::vector<int> channel_plan, std::string pairing_ssid)
	{
		StopPairChannelSweep();
		if (!m_pair_channel_sweep || channel_plan.size() < 2)
			return;

		auto current = std::find(channel_plan.begin(), channel_plan.end(), m_channel);
		size_t channel_index = current == channel_plan.end()
			? 0
			: (static_cast<size_t>(std::distance(channel_plan.begin(), current)) + 1) % channel_plan.size();
		m_pair_channel_sweep_stop.store(false);
		m_pair_channel_sweep_thread = std::thread([this, channel_plan = std::move(channel_plan), pairing_ssid = std::move(pairing_ssid), channel_index]() mutable {
			Log("pair-sweep: live rotation started; dwellMs=" + std::to_string(m_pair_channel_dwell_ms));
			while (!m_pair_channel_sweep_stop.load() && !m_pairing_activity_seen.load() && !is_stop_requested())
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_pair_channel_dwell_ms);
				int last_countdown = -1;
				while (std::chrono::steady_clock::now() < deadline &&
					!m_pair_channel_sweep_stop.load() && !m_pairing_activity_seen.load() && !is_stop_requested())
				{
					const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
						deadline - std::chrono::steady_clock::now());
					const int countdown = static_cast<int>((remaining.count() + 999) / 1000);
					if (countdown >= 1 && countdown <= 3 && countdown != last_countdown)
					{
						Log("pair-sweep: channel " + std::to_string(m_channel) +
							" switching in " + std::to_string(countdown) + "...");
						last_countdown = countdown;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}
				if (m_pair_channel_sweep_stop.load() || m_pairing_activity_seen.load() || is_stop_requested())
					break;

				const int next_channel = channel_plan[channel_index];
				channel_index = (channel_index + 1) % channel_plan.size();
				Log("pair-sweep: switching from channel " + std::to_string(m_channel) +
					" to " + std::to_string(next_channel));
				StopHostapd();
				if (m_pair_channel_sweep_stop.load() || is_stop_requested())
					break;

				std::string error;
				if (!WritePairingHostapdConfig(
						m_ap_interface,
						m_ap_mac,
						pairing_ssid,
						m_credential_blob_path,
						next_channel,
						m_pair_ignore_broadcast_probe_requests,
						m_pairing_config_path,
						error) ||
					!StartHostapd(m_pairing_config_path, error) ||
					!WaitForHostapdReady("pairing-sweep", std::chrono::seconds(20), error))
				{
					Log("pair-sweep: channel " + std::to_string(next_channel) + " failed: " + error);
					StopHostapd();
					continue;
				}

				m_channel = next_channel;
				if (!ArmWpsPinAny(error))
				{
					Log("pair-sweep: channel " + std::to_string(next_channel) + " WPS_PIN failed: " + error);
					StopHostapd();
					continue;
				}
				m_pairing_activity_seen.store(false);
				StartHostapdMonitor("pairing");
				Log("pair-sweep: channel " + std::to_string(next_channel) + " ready");
			}

			if (m_pairing_activity_seen.load())
				Log("pair-sweep: pairing activity detected; locked on channel " + std::to_string(m_channel));
			else
				Log("pair-sweep: live rotation stopped");
		});
	}

	void StopPairChannelSweep()
	{
		m_pair_channel_sweep_stop.store(true);
		if (m_pair_channel_sweep_thread.joinable())
			m_pair_channel_sweep_thread.join();
	}

	void MonitorHostapdOutputLoop()
	{
		std::string read_buffer;
		while (!m_hostapd_monitor_stop.load() && !is_stop_requested())
		{
			const int fd = m_hostapd_output_fd;
			if (fd < 0)
				break;

			fd_set set;
			FD_ZERO(&set);
			FD_SET(fd, &set);
			timeval tv{};
			tv.tv_sec = 0;
			tv.tv_usec = 250000;
			const int ready = select(fd + 1, &set, nullptr, nullptr, &tv);
			if (ready < 0)
			{
				if (errno == EINTR)
					continue;
				if (errno != EBADF)
					Log("hostapd monitor: select failed: " + std::string(std::strerror(errno)));
				break;
			}
			if (ready == 0 || !FD_ISSET(fd, &set))
				continue;

			std::array<char, 2048> block{};
			const auto read_size = read(fd, block.data(), block.size());
			if (read_size > 0)
			{
				read_buffer.append(block.data(), static_cast<size_t>(read_size));
				AppendCapped(m_hostapd_log, std::string_view(block.data(), static_cast<size_t>(read_size)), 64 * 1024);
			}
			else if (read_size == 0)
			{
				Log("hostapd monitor: EOF");
				break;
			}
			else
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
					continue;
				if (errno != EBADF)
					Log("hostapd monitor: read failed: " + std::string(std::strerror(errno)));
				break;
			}

			size_t nl_pos = std::string::npos;
			while ((nl_pos = read_buffer.find('\n')) != std::string::npos)
			{
				std::string line = Trim(read_buffer.substr(0, nl_pos));
				read_buffer.erase(0, nl_pos + 1);
				if (line.empty())
					continue;
				LogHostapdLine(m_hostapd_monitor_phase, line);
			}
		}
		Log("hostapd monitor: stopped");
	}

	void CleanupTemporaryFiles()
	{
		if (!m_credential_blob_path.empty())
		{
			std::error_code ec;
			std::filesystem::remove(m_credential_blob_path, ec);
			if (!ec)
				Log("cleanup: removed " + m_credential_blob_path);
			m_credential_blob_path.clear();
		}
		if (!m_pairing_config_path.empty())
		{
			std::error_code ec;
			std::filesystem::remove(m_pairing_config_path, ec);
			if (!ec)
				Log("cleanup: removed " + m_pairing_config_path);
			m_pairing_config_path.clear();
		}
		if (!m_runtime_config_path.empty())
		{
			std::error_code ec;
			std::filesystem::remove(m_runtime_config_path, ec);
			if (!ec)
				Log("cleanup: removed " + m_runtime_config_path);
			m_runtime_config_path.clear();
		}
	}

	bool WaitForHostapdReady(std::string_view phase, std::chrono::seconds timeout, std::string& error)
	{
		auto deadline = std::chrono::steady_clock::now() + timeout;
		std::string read_buffer;

		while (std::chrono::steady_clock::now() < deadline)
		{
			if (is_stop_requested())
			{
				error = "operation cancelled";
				return false;
			}
			if (m_hostapd_output_fd < 0 || m_hostapd_pid <= 0)
			{
				error = "hostapd is not running";
				return false;
			}

			fd_set set;
			FD_ZERO(&set);
			FD_SET(m_hostapd_output_fd, &set);
			timeval tv{};
			tv.tv_sec = 0;
			tv.tv_usec = 250000;
			const int ready = select(m_hostapd_output_fd + 1, &set, nullptr, nullptr, &tv);
			if (ready > 0 && FD_ISSET(m_hostapd_output_fd, &set))
			{
				std::array<char, 2048> block{};
				const auto read_size = read(m_hostapd_output_fd, block.data(), block.size());
				if (read_size > 0)
				{
					read_buffer.append(block.data(), static_cast<size_t>(read_size));
					AppendCapped(m_hostapd_log, std::string_view(block.data(), static_cast<size_t>(read_size)), 64 * 1024);
				}
				else if (read_size == 0)
				{
					int status = 0;
					const pid_t pid = m_hostapd_pid;
					const auto wait_res = waitpid(pid, &status, WNOHANG);
					if (wait_res == pid)
					{
						m_hostapd_pid = -1;
						if (WIFEXITED(status))
						{
							const int exit_code = WEXITSTATUS(status);
							std::ostringstream out;
							out << "hostapd exited before AP became ready (exit " << exit_code << ")";
							if (exit_code == 127)
								out << "; verify DRCD_HOSTAPD_BIN=" << Quote(m_hostapd_binary) << " is executable";
							error = out.str();
						}
						else if (WIFSIGNALED(status))
						{
							error = "hostapd exited before AP became ready (signal " +
								std::to_string(WTERMSIG(status)) + ")";
						}
						else
						{
							error = "hostapd exited before AP became ready";
						}
					}
					else
					{
						error = "hostapd exited before AP became ready";
					}
					const auto tail = TailLines(m_hostapd_log, 24);
					if (!tail.empty())
						error += " | " + tail;
					return false;
				}
			}

			size_t nl_pos = std::string::npos;
				while ((nl_pos = read_buffer.find('\n')) != std::string::npos)
				{
					std::string line = Trim(read_buffer.substr(0, nl_pos));
					read_buffer.erase(0, nl_pos + 1);
					if (line.empty())
						continue;
					LogHostapdLine(std::string(phase), line);

				if (line.find("AP-ENABLED") != std::string::npos || line.find("AP_ENABLED") != std::string::npos)
					return true;
				if (line.find("Failed to initialize") != std::string::npos ||
					line.find("Could not configure driver mode") != std::string::npos ||
					line.find("Could not set channel") != std::string::npos ||
					line.find("Operation not permitted") != std::string::npos ||
					line.find("Permission denied") != std::string::npos)
				{
					error = "hostapd initialization failed";
					const auto tail = TailLines(m_hostapd_log, 16);
					if (!tail.empty())
						error += " (" + tail + ")";
					return false;
				}
				if (line.find("NL80211_CMD_STOP_AP") != std::string::npos ||
					line.find("Interface " + m_ap_interface + " is unavailable -- stopped") != std::string::npos)
				{
					error = std::string(phase) + " AP was stopped by the driver";
					return false;
				}
			}

			int status = 0;
			const auto wait_res = waitpid(m_hostapd_pid, &status, WNOHANG);
			if (wait_res == m_hostapd_pid)
				{
					m_hostapd_pid = -1;
					std::ostringstream out;
					out << "hostapd terminated unexpectedly";
					if (WIFEXITED(status))
						out << " (exit " << WEXITSTATUS(status) << ")";
					else if (WIFSIGNALED(status))
						out << " (signal " << WTERMSIG(status) << ")";
					error = out.str();
					const auto tail = TailLines(m_hostapd_log, 24);
					if (!tail.empty())
						error += " (" + tail + ")";
					return false;
			}
		}

		error = "timed out waiting for hostapd AP-ENABLED";
		return false;
	}

	bool SendHostapdControlCommand(const std::string& command, std::string& response)
	{
		const std::string server_path = std::string(kHostapdControlPath) + "/" + m_ap_interface;
		if (server_path.size() >= sizeof(sockaddr_un{}.sun_path))
		{
			response = "hostapd control path too long";
			return false;
		}

		const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
		if (fd < 0)
		{
			response = "socket() failed";
			return false;
		}

		const std::string client_path = BuildTempPath("ctrl") + ".sock";
		if (client_path.size() >= sizeof(sockaddr_un{}.sun_path))
		{
			close(fd);
			response = "client control path too long";
			return false;
		}

		sockaddr_un local{};
		local.sun_family = AF_UNIX;
		std::snprintf(local.sun_path, sizeof(local.sun_path), "%s", client_path.c_str());
		::unlink(local.sun_path);
		if (bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
		{
			close(fd);
			response = "bind() failed";
			return false;
		}

		sockaddr_un remote{};
		remote.sun_family = AF_UNIX;
		std::snprintf(remote.sun_path, sizeof(remote.sun_path), "%s", server_path.c_str());
		if (connect(fd, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) != 0)
		{
			::unlink(local.sun_path);
			close(fd);
			response = "connect() failed";
			return false;
		}

		const auto sent = send(fd, command.data(), command.size(), 0);
		if (sent < 0)
		{
			::unlink(local.sun_path);
			close(fd);
			response = "send() failed";
			return false;
		}

		fd_set set;
		FD_ZERO(&set);
		FD_SET(fd, &set);
		timeval tv{};
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		const int ready = select(fd + 1, &set, nullptr, nullptr, &tv);
		if (ready <= 0 || !FD_ISSET(fd, &set))
		{
			::unlink(local.sun_path);
			close(fd);
			response = "hostapd control timeout";
			return false;
		}

		std::array<char, 512> buffer{};
		const auto n = recv(fd, buffer.data(), buffer.size() - 1, 0);
		if (n < 0)
		{
			::unlink(local.sun_path);
			close(fd);
			response = "recv() failed";
			return false;
		}

		buffer[static_cast<size_t>(n)] = '\0';
		response = Trim(std::string(buffer.data()));
		::unlink(local.sun_path);
		close(fd);
		return true;
	}

	std::string BuildWpsPin() const
	{
		std::ostringstream pin;
		pin << std::setw(4) << std::setfill('0') << m_pairing_code.numeric() << "5678";
		return pin.str();
	}

	std::string BuildChecksumCorrectedWpsPin(const std::string& pin8) const
	{
		if (pin8.size() != 8 || !std::all_of(pin8.begin(), pin8.end(), [](char ch) {
				return std::isdigit(static_cast<unsigned char>(ch)) != 0;
			}))
		{
			return pin8;
		}

		unsigned int seven_digit_pin = static_cast<unsigned int>(std::stoul(pin8.substr(0, 7)));
		unsigned int accum = 0;
		unsigned int value = seven_digit_pin;
		while (value != 0)
		{
			accum += 3 * (value % 10);
			value /= 10;
			accum += value % 10;
			value /= 10;
		}
		const unsigned int checksum = (10 - (accum % 10)) % 10;
		return pin8.substr(0, 7) + static_cast<char>('0' + checksum);
	}

	bool ArmWpsPinCommand(const std::string& command, const std::string& label, std::string& error)
	{
		for (int attempt = 0; attempt < 8; ++attempt)
		{
			if (is_stop_requested())
			{
				error = "operation cancelled";
				return false;
			}
			std::string response;
			Log("hostapd-ctrl: " + label + " attempt=" + std::to_string(attempt + 1));
			if (SendHostapdControlCommand(command, response))
			{
				Log("hostapd-ctrl: " + label + " response='" + response + "'");
				if (response.find("FAIL") != std::string::npos)
				{
					error = response;
					return false;
				}
				return true;
			}

			error = response;
			Log("hostapd-ctrl: " + label + " transport failure='" + response + "'");
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		if (error.empty())
			error = "no response from hostapd control socket";
		return false;
	}

	bool ArmWpsPinAny(std::string& error)
	{
		const std::string pin = BuildWpsPin();
		const std::string command = "WPS_PIN any " + pin + " " + std::to_string(m_wps_pin_timeout_seconds);
		if (ArmWpsPinCommand(command, "WPS_PIN any", error))
			return true;

		const std::string corrected_pin = BuildChecksumCorrectedWpsPin(pin);
		if (corrected_pin == pin)
			return false;

		Log("hostapd-ctrl: WPS_PIN any fallback using checksum-corrected pin '" + corrected_pin + "'");
		const std::string fallback_command = "WPS_PIN any " + corrected_pin + " " + std::to_string(m_wps_pin_timeout_seconds);
		return ArmWpsPinCommand(fallback_command, "WPS_PIN any (checksum)", error);
	}

	void MaybeArmTargetedWpsPin(std::string_view phase, std::string_view mac)
	{
		if (IsLocallyAdministeredMac(mac))
			return;
		if (phase != "pairing")
			return;
		if (m_hostapd_pid <= 0 || m_hostapd_output_fd < 0)
			return;

		const auto now = std::chrono::steady_clock::now();
		const auto mac_key = std::string(mac);
		const auto last_it = m_targeted_wps_pin_at.find(mac_key);
		if (last_it != m_targeted_wps_pin_at.end() &&
			now - last_it->second < std::chrono::seconds(30))
		{
			return;
		}

		m_targeted_wps_pin_at[mac_key] = now;
		std::string error;
		const std::string command = "WPS_PIN " + mac_key + " " + BuildWpsPin() + " " + std::to_string(m_wps_pin_timeout_seconds);
		if (ArmWpsPinCommand(command, "WPS_PIN " + mac_key, error))
		{
			Log("pairing-candidate: targeted WPS PIN armed for " + mac_key);
		}
		else
		{
			Log("pairing-candidate: targeted WPS PIN failed for " + mac_key + ": " + error);
		}
	}

	bool IsImportantHostapdLine(std::string_view line) const
	{
		if (m_hostapd_raw_logging)
			return true;

		constexpr std::array<std::string_view, 13> kEventNeedles{
			"AP-ENABLED",
			"AP_ENABLED",
			"AP-DISABLED",
			"AP_DISABLED",
			"AP-STA-CONNECTED",
			"AP-STA-DISCONNECTED",
			"WPS-",
			"WPS-PIN-NEEDED",
			"CTRL-EVENT",
			"NL80211_CMD_STOP_AP",
			"INTERFACE_UNAVAILABLE",
			"Interface initialization failed",
			"is unavailable -- stopped",
		};
		for (const auto needle : kEventNeedles)
		{
			if (line.find(needle) != std::string::npos)
				return true;
		}

		constexpr std::array<std::string_view, 12> kFailureNeedles{
			"Failed",
			"failed",
			"ERROR",
			"error",
			"Could not",
			"Permission denied",
			"Operation not permitted",
			"Device or resource busy",
			"timed out",
			"Address already in use",
			"No such file",
			"deinit ifname",
		};
		for (const auto needle : kFailureNeedles)
		{
			if (line.find(needle) != std::string::npos)
				return true;
		}

		return false;
	}

	static std::optional<std::string> ExtractProbeSourceMac(std::string_view line)
	{
		if (line.find("stype=4 (WLAN_FC_STYPE_PROBE_REQ)") == std::string::npos)
			return std::nullopt;

		const size_t sa_pos = line.find(" sa=");
		if (sa_pos == std::string::npos)
			return std::nullopt;
		const size_t mac_start = sa_pos + 4;
		if (mac_start + 17 > line.size())
			return std::nullopt;

		const std::string mac(line.substr(mac_start, 17));
		if (!std::regex_match(mac, std::regex(R"(^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$)")))
			return std::nullopt;
		return mac;
	}

	void MaybeLogProbeSourceMac(std::string_view phase, std::string_view line)
	{
		const auto mac = ExtractProbeSourceMac(line);
		if (!mac.has_value())
			return;

		const auto now = std::chrono::steady_clock::now();
		const auto last_it = m_probe_seen.find(*mac);
		if (last_it != m_probe_seen.end() &&
			now - last_it->second < std::chrono::seconds(5))
		{
			return;
		}

		m_probe_seen[*mac] = now;
		const bool locally_administered = IsLocallyAdministeredMac(*mac);
		Log("probe-seen: sa=" + *mac + (locally_administered ? " local-admin=1" : " local-admin=0"));
		if (!locally_administered)
			MaybeArmTargetedWpsPin(phase, *mac);
	}

	void LogHostapdLine(const std::string& phase, const std::string& line)
	{
		const bool received_pairing_frame = line.find("RX frame") != std::string::npos &&
			(line.find("WLAN_FC_STYPE_AUTH") != std::string::npos ||
			 line.find("WLAN_FC_STYPE_ASSOC_REQ") != std::string::npos ||
			 line.find("WLAN_FC_STYPE_REASSOC_REQ") != std::string::npos);
		const bool pairing_protocol_event =
			line.find("AP-STA-CONNECTED") != std::string::npos ||
			line.find("WPS-REG-SUCCESS") != std::string::npos ||
			line.find("WPS-SUCCESS") != std::string::npos ||
			line.find("WPS-FAIL") != std::string::npos;
		if (phase == "pairing" && (received_pairing_frame || pairing_protocol_event))
		{
			m_pairing_activity_seen.store(true);
		}
		if (phase == "pairing" && line.find("WPS-SUCCESS") != std::string::npos)
		{
			Log("pairing-complete: WPS succeeded; scheduling automatic runtime transition");
			m_pairing_complete_requested.store(true);
		}
		// Association is only link-layer readiness. Do not call the GamePad
		// connected until the DRC command/HID protocol produces valid traffic.
		if (line.find("AP-STA-DISCONNECTED") != std::string::npos &&
			m_runtime_transport && m_runtime_transport->stats().protocol_ready)
			m_gamepad_disconnected_requested.store(true);
		if (line.find("INTERFACE_UNAVAILABLE") != std::string::npos ||
			line.find("is unavailable -- stopped") != std::string::npos)
			QueueStatus("Wi-Fi AP was stopped externally; see " + m_log_path);
		MaybeLogProbeSourceMac(phase, line);

		const std::string message = "hostapd[" + phase + "]: " + line;
		{
			std::lock_guard lock(m_log_mutex);
			if (m_log_stream.good())
				m_log_stream << "drcd-backend: " << message << std::endl;
		}
		if (m_verbose_logging && IsImportantHostapdLine(line))
			std::cerr << "drcd-backend: " << message << std::endl;
	}

	void Log(const std::string& message) const
	{
		{
			std::lock_guard lock(m_log_mutex);
			if (m_log_stream.good())
				m_log_stream << "drcd-backend: " << message << std::endl;
		}
		if (m_verbose_logging)
			std::cerr << "drcd-backend: " << message << std::endl;
	}

private:
	BackendSnapshot m_snapshot;
	std::string m_hostapd_binary;
	bool m_use_pkexec = false;
	int m_channel = 36;
	bool m_verbose_logging = true;
	bool m_hostapd_raw_logging = false;
	bool m_pair_ignore_broadcast_probe_requests = false;
	bool m_pair_channel_sweep = true;
	std::string m_pair_channel_list_override;
	int m_pair_channel_dwell_ms = 20000;
	int m_wps_pin_timeout_seconds = kDefaultWpsPinTimeoutSeconds;
	std::thread m_pair_channel_sweep_thread;
	std::atomic_bool m_pair_channel_sweep_stop{false};
	std::atomic_bool m_pairing_activity_seen{false};
	std::atomic_bool m_pairing_complete_requested{false};
	std::atomic_bool m_gamepad_disconnected_requested{false};
	std::mutex m_status_mutex;
	std::deque<std::string> m_status_events;
	std::string m_log_path;
	std::string m_credentials_path;
	mutable std::mutex m_log_mutex;
	mutable std::ofstream m_log_stream;

	pid_t m_hostapd_pid = -1;
	int m_hostapd_output_fd = -1;
	std::string m_hostapd_log;
	std::thread m_hostapd_monitor_thread;
	std::atomic_bool m_hostapd_monitor_stop{false};
	std::string m_hostapd_monitor_phase;

	std::string m_base_interface;
	std::string m_ap_interface;
	SavedInterfaceState m_saved_interface;
	bool m_network_manager_claimed = false;
	bool m_tsf_monitor_created = false;
	bool m_per_tid_rts_enabled = false;
	bool m_global_rts_enabled = false;
	bool m_fixed_runtime_bitrate_enabled = false;
	std::string m_runtime_phy_name;
	drc_host::MacAddress m_ap_mac{};
	drc_host::PairingCode m_pairing_code{};
	std::string m_runtime_ssid;
	std::string m_psk_hex;
	std::string m_test_media_path;
	bool m_test_black_frames = false;
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_probe_seen;
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_targeted_wps_pin_at;

	std::string m_credential_blob_path;
	std::string m_pairing_config_path;
	std::string m_runtime_config_path;
	int m_dhcp_socket = -1;
	std::thread m_dhcp_thread;
	std::atomic_bool m_dhcp_stop{false};
	std::unique_ptr<drc_host::RuntimeTransport> m_runtime_transport;
	std::unique_ptr<MediaStreamer> m_media_streamer;
};
}

std::unique_ptr<SessionBackend> create_default_session_backend()
{
	return std::make_unique<LinuxSessionBackend>();
}
}
