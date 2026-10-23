#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace {

class Watchdog final
{
public:
	Watchdog(): m_thread([this] {
		std::unique_lock<std::mutex> lock(m_mutex);
		if (!m_changed.wait_for(lock, std::chrono::seconds(10), [this] { return m_done; }))
		{
			std::fprintf(stderr, "LogFileShutdownTest: FAIL: concurrent shutdown timed out\n");
			std::fflush(stderr);
			std::_Exit(2);
		}
	}) {}

	~Watchdog()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_done = true;
		}
		m_changed.notify_one();
		m_thread.join();
	}

	Watchdog(const Watchdog&)            = delete;
	Watchdog& operator=(const Watchdog&) = delete;

private:
	std::mutex              m_mutex;
	std::condition_variable m_changed;
	bool                    m_done = false;
	std::thread             m_thread;
};

class TemporaryDirectory final
{
public:
	TemporaryDirectory()
	{
		std::error_code error;
		const auto root = std::filesystem::temp_directory_path(error);
		if (error) return;
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		for (unsigned attempt = 0; attempt < 100; ++attempt)
		{
			auto candidate = root / ("lumen-log-shutdown-" + std::to_string(stamp) + "-" + std::to_string(attempt));
			if (std::filesystem::create_directory(candidate, error))
			{
				m_path = std::move(candidate);
				break;
			}
			error.clear();
		}
	}

	~TemporaryDirectory()
	{
		if (!m_path.empty())
		{
			std::error_code error;
			std::filesystem::remove_all(m_path, error);
		}
	}

	const std::filesystem::path& Path() const { return m_path; }

	TemporaryDirectory(const TemporaryDirectory&)            = delete;
	TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

private:
	std::filesystem::path m_path;
};

bool ReadFile(const std::filesystem::path& path, std::string* contents)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) return false;
	contents->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	return !file.bad();
}

bool RunRound(const std::filesystem::path& path, unsigned round)
{
	Lumen::Log::SetOutputFile(Lumen::String::FromUtf8(path.string().c_str()));
	Lumen::printf("normal-record=%u\n", round);

	constexpr unsigned WRITER_COUNT = 6;
	std::atomic<unsigned> ready {0};
	std::atomic<unsigned> first_records {0};
	std::atomic<bool> start {false};
	std::atomic<bool> closed {false};
	const std::string payload(4096, 'x');
	std::array<std::thread, WRITER_COUNT> writers;
	for (unsigned index = 0; index < WRITER_COUNT; ++index)
	{
		writers[index] = std::thread([&, index] {
			ready.fetch_add(1, std::memory_order_release);
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			Lumen::printf("writer=%u first-record\n", index);
			first_records.fetch_add(1, std::memory_order_release);
			// Keep writers contending with the closer. The timing-sensitive path
			// uses the public API without access to the logger's private mutex.
			while (!closed.load(std::memory_order_acquire))
			{
				Lumen::printf("writer=%u %s\n", index, payload.c_str());
			}
			Lumen::printf("after-close-worker=%u\n", index);
		});
	}
	while (ready.load(std::memory_order_acquire) != WRITER_COUNT) std::this_thread::yield();
	start.store(true, std::memory_order_release);
	while (first_records.load(std::memory_order_acquire) != WRITER_COUNT) std::this_thread::yield();
	Lumen::Log::LogSubsystem::Instance()->UnexpectedShutdown(nullptr);
	closed.store(true, std::memory_order_release);
	for (auto& writer: writers) writer.join();

	std::string before;
	// The file sink's normal UTF-8 output starts with its encoding marker.
	const auto expected = std::string("\xef\xbb\xbf", 3) + "normal-record=" + std::to_string(round) + "\n";
	if (!ReadFile(path, &before) || before.compare(0, expected.size(), expected) != 0 ||
	    before.find("after-close") != std::string::npos)
	{
		std::fprintf(stderr, "LogFileShutdownTest: FAIL: round %u lost a record or wrote after close\n", round);
		return false;
	}
	for (unsigned index = 0; index < WRITER_COUNT; ++index)
	{
		if (before.find("writer=" + std::to_string(index) + " first-record\n") == std::string::npos)
		{
			std::fprintf(stderr, "LogFileShutdownTest: FAIL: round %u did not flush writer %u\n", round, index);
			return false;
		}
	}

	Lumen::printf("after-close-main\n");
	Lumen::Log::LogSubsystem::Instance()->UnexpectedShutdown(nullptr);
	std::string after;
	if (!ReadFile(path, &after) || after != before)
	{
		std::fprintf(stderr, "LogFileShutdownTest: FAIL: round %u changed the closed file\n", round);
		return false;
	}
	return true;
}

} // namespace

int main()
{
	Watchdog watchdog;
	TemporaryDirectory temporary;
	if (temporary.Path().empty())
	{
		std::fprintf(stderr, "LogFileShutdownTest: FAIL: cannot create a temporary directory\n");
		return 1;
	}

	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::File);
	const auto path = temporary.Path() / "output.txt";
	for (unsigned round = 0; round < 32; ++round)
	{
		if (!RunRound(path, round)) return 1;
	}
	Lumen::Log::LogSubsystem::Instance()->Destroy(nullptr);
	std::puts("LogFileShutdownTest: PASS");
	return 0;
}
