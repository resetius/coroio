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
    TRingActor(size_t idx, size_t n, size_t m, std::vector<TActorId>& r)
      : Idx_(idx)
      , N_(n)
      , Remain_(m)
      , M_(m)
      , Ring_(r)
    { }

    TFuture<void> Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        if (Idx_ == 0 && Remain_ == M_) [[unlikely]] {
            StartTime_ = std::chrono::steady_clock::now();
        }

        if (Idx_ == 0 && Remain_ == 0) [[unlikely]] {
            co_return;
        }

        ctx->Send(Ring_[(Idx_ + 1)%N_], TNext{});

        if (Idx_ == 0) {
            --Remain_;
            PrintProgress();

            if (Remain_ == 0) {
                ShutdownRing(ctx);
            }
        }

        co_return;
    }

private:
    void ShutdownRing(TActorContext::TPtr& ctx) {
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - StartTime_).count();
        std::cout << "\nRing throughput: "
                    << (double)(N_ * M_) / secs
                    << " msg/s\n";
        for (auto& idx : Ring_) {
            ctx->Send(idx, TPoison{});
        }
    }

    void PrintProgress() {
        size_t processed = M_ - Remain_;
        int percent = int((processed * 100) / M_);
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

    size_t Idx_;
    size_t N_;
    size_t Remain_;
    size_t M_;
    std::vector<TActorId>& Ring_;
    std::chrono::steady_clock::time_point StartTime_;
    int LastPercent_ = -1;
};

int main(int argc, char** argv) {
    size_t N = 2; // Number of actors
    size_t M = 100; // Messages
    std::string mode = "throughput";

    TLoop<TDefaultPoller> loop;
    TActorSystem sys(&loop.Poller());
    std::vector<TActorId> ringIds;

    for (int i = 0; i < argc; ++i) {
        if (std::string(argv[i]) == "--actors") {
            if (i + 1 < argc) {
                N = std::stoul(argv[++i]);
            }
        } else if (std::string(argv[i]) == "--messages") {
            if (i + 1 < argc) {
                M = std::stoul(argv[++i]);
            }
        } else if (std::string(argv[i]) == "--mode") {
            if (i + 1 < argc) {
                mode = argv[++i];
            }
        }
    }

    if (mode == "throughput") {
        ringIds.resize(N);
        for (size_t i = 0; i < N; ++i) {
            ringIds[i] = sys.Register(std::make_unique<TRingActor>(i, N, M, ringIds));
        }
        sys.Send(ringIds[0], ringIds[0], TNext{});
    } else {
        // Unsupported yet
    }

    sys.Serve();
    sys.MaybeNotify();
    while (sys.ActorsSize() > 0) {
        loop.Step();
    }
}
