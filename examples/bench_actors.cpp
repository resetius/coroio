#include <chrono>
#include <iostream>
#include <vector>
#include <coroio/all.hpp>
#include <coroio/corochain.hpp>
#include <coroio/actors/actorsystem.hpp>

#include <cstdint>

using namespace NNet;
using namespace NNet::NActors;

struct TNext {
    static constexpr TMessageId MessageId = 100;
};

class TRingActor : public IActor {
public:
    TRingActor(size_t idx, size_t totalMessages, std::vector<TActorId>& r)
      : Idx_(idx)
      , Remain_(totalMessages)
      , TotalMessages_(totalMessages)
      , Ring_(r)
    { }

    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        if (Idx_ == 0 && !TimerStarted_) [[unlikely]] {
            StartTime_ = std::chrono::steady_clock::now();
            TimerStarted_ = true;
        }

        if (Idx_ == 0 && Remain_ == 0) [[unlikely]] {
            return;
        }

        ctx->Send<TNext>(Ring_[(Idx_ + 1)%Ring_.size()]);

        if (Idx_ == 0) {
            if (ctx->Sender()) {
                --Remain_;
                PrintProgress();
            }

            if (Remain_ == 0) {
                ShutdownRing(ctx);
            }
        }

        return;
    }

private:
    void ShutdownRing(TActorContext::TPtr& ctx) {
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - StartTime_).count();
        std::cout << "\nRing throughput: "
                    << (double)(TotalMessages_) / secs
                    << " msg/s\n";
        for (auto& idx : Ring_) {
            ctx->Send<TPoison>(idx);
        }
    }

    void PrintProgress() {
        size_t processed = TotalMessages_ - Remain_;
        int percent = int((processed * 100) / TotalMessages_);
        if (percent != LastPercent_) {
            LastPercent_ = percent;
            const int barWidth = 50;
            int pos = (percent * barWidth) / 100;
            std::cerr << "\r[";
            for (int i = 0; i < barWidth; ++i) {
                if (i < pos) {
                    std::cerr << "=";
                }
                else if (i == pos) {
                    std::cerr << ">";
                }
                else {
                    std::cerr << " ";
                }
            }
            std::cerr << "] " << percent << "%";
            std::cerr.flush();
        }
    }

    bool TimerStarted_ = false;
    size_t Idx_;
    size_t Remain_;
    size_t TotalMessages_;
    std::vector<TActorId>& Ring_;
    std::chrono::steady_clock::time_point StartTime_;
    int LastPercent_ = -1;
};

int main(int argc, char** argv) {
    size_t numActors = 2; // Number of actors
    size_t totalMessages = 100; // Messages
    size_t batchSize = 1; // Batch size
    std::string mode = "throughput";

    TLoop<TDefaultPoller> loop;
    TActorSystem sys(&loop.Poller());
    std::vector<TActorId> ringIds;

    for (int i = 0; i < argc; ++i) {
        if (std::string(argv[i]) == "--actors") {
            if (i + 1 < argc) {
                numActors = std::stoul(argv[++i]);
            }
        } else if (std::string(argv[i]) == "--messages") {
            if (i + 1 < argc) {
                totalMessages = std::stoul(argv[++i]);
            }
        } else if (std::string(argv[i]) == "--batch") {
            if (i + 1 < argc) {
                batchSize = std::stoul(argv[++i]);
            }
        } else if (std::string(argv[i]) == "--mode") {
            if (i + 1 < argc) {
                mode = argv[++i];
            }
        }
    }

    ringIds.resize(numActors);
    for (size_t i = 0; i < numActors; ++i) {
        ringIds[i] = sys.Register(std::make_unique<TRingActor>(i, totalMessages, ringIds));
    }
    for (int i = 0; i < batchSize; i++) {
        sys.Send<TNext>(TActorId{}, ringIds[0]);
    }

    sys.Serve();

    while (sys.ActorsSize() > 0) {
        loop.Step();
    }
}
