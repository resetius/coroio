#pragma once

#ifdef __linux__

#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <functional>

class TPerfWrapper {
public:
    TPerfWrapper(const std::string& outputFile = "perf.data",
                 const std::vector<std::string>& events = {"cycles", "instructions"})
        : OutputFile(outputFile), Events(events) {}

    template<typename Func>
    void Profile(Func&& func) {
        pid_t perfPid = StartPerf();

        usleep(2000000); // 2000ms

        func();

        StopPerf(perfPid);
    }

private:
    std::string OutputFile;
    std::vector<std::string> Events;

    pid_t StartPerf() {
        pid_t pid = fork();
        if (pid == 0) {
            std::vector<char*> args;
            args.push_back(const_cast<char*>("perf"));
            args.push_back(const_cast<char*>("record"));
            args.push_back(const_cast<char*>("-o"));
            args.push_back(const_cast<char*>(OutputFile.c_str()));

            for (const auto& event : Events) {
                args.push_back(const_cast<char*>("-e"));
                args.push_back(const_cast<char*>(event.c_str()));
            }
            args.push_back(const_cast<char*>("-F"));
            args.push_back(const_cast<char*>("9999"));
            //args.push_back(const_cast<char*>("-g"));
            //args.push_back(const_cast<char*>("--call-graph=dwarf"));
            //args.push_back(const_cast<char*>("-e"));
            //args.push_back(const_cast<char*>("cache-misses"));
            //args.push_back(const_cast<char*>("-e"));
            //args.push_back(const_cast<char*>("cache-references"));

            args.push_back(const_cast<char*>("-p"));
            std::string parentPid = std::to_string(getppid());
            args.push_back(const_cast<char*>(parentPid.c_str()));

            args.push_back(nullptr);

            execvp("perf", args.data());
            _exit(1);
        }
        return pid;
    }

    void StopPerf(pid_t perfPid) {
        if (perfPid > 0) {
            kill(perfPid, SIGTERM);
            int status;
            waitpid(perfPid, &status, 0);
        }
    }
};

#else
class TPerfWrapper {
public:
    TPerfWrapper(const std::string& outputFile = "perf.data",
                 const std::vector<std::string>& events = {"cycles", "instructions"}) {}

    template<typename Func>
    void Profile(Func&& func) {
        func();
    }
};

#endif
