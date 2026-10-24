#pragma once
// Client-side updater. Asks the GitHub Releases API of the project for the latest release, compares its tag with the version built into
// this executable, and, when the player agrees, downloads the new executables, checks their SHA-256 against the digest GitHub
// publishes for each release asset, swaps them in (the running exe is renamed aside, which Windows allows) and the caller restarts.
// Nothing runs on a server of ours. Release assets it understands (docs/RELEASING.md):
//   Crash2.exe          replaces the running executable, whatever it is called
//   Crash2-Server.exe   replaces "Crash2 Server.exe" next to it, if that file exists
// Everything that touches the network runs in a background thread; the game never waits for it.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cb2update {

enum class State { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed };

// True when version a ("v1.2.0" or "1.2") is newer than b. Missing parts count as 0; anything after '-' is ignored.
bool isNewer(const std::string& a, const std::string& b);

// Removes what a previous update left behind (*.old next to the executable, unfinished *.new). Call at startup.
void cleanLeftovers();

class Updater {
public:
    Updater() = default;
    ~Updater() { join(); }
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // `repo` is "owner/name" on GitHub. Does nothing while a check or download is running.
    void startCheck(const std::string& repo, const std::string& currentVersion);
    void startDownload();                       // after state() == Available
    bool install(std::string& error);          // after state() == Ready: swaps the files; the caller then restarts the game

    State state() const { return st; }
    int percent() const { return pct; }         // while Downloading
    std::string latest() const;                 // "0.2.0" once known
    std::string error() const;                  // after Failed
    std::string notes() const;                  // release description (first lines), may be empty

private:
    struct Job {
        std::string name, url, sha256;
        uint64_t size = 0;
        std::wstring target, temp;
    };
    void join() { if (worker.joinable()) worker.join(); }
    void check(std::string repo, std::string current);
    void download();
    void fail(const std::string& why);

    std::atomic<State> st{State::Idle};
    std::atomic<int> pct{0};
    mutable std::mutex mu;
    std::string latestVersion, errorText, notesText;
    std::vector<Job> jobs;
    std::thread worker;
};

}  // namespace cb2update
