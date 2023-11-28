namespace NNet {

template<typename TPoller>
class TLoop {
public:
    void Loop() {
        while (Running_) {
            Step();
        }
    }

    void Stop() {
        Running_ = false;
    }

    void Step() {
        Poller_.Poll();
        Poller_.WakeupReadyHandles();
    }

    TPoller& Poller() {
        return Poller_;
    }

private:
    TPoller Poller_;
    bool Running_ = true;
};

} // namespace NNet
