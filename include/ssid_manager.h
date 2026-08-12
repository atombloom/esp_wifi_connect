#ifndef SSID_MANAGER_H
#define SSID_MANAGER_H

#include <string>
#include <vector>
#include <mutex>

struct SsidItem {
    std::string ssid;
    std::string password;
};

class SsidManager {
public:
    static SsidManager& GetInstance() {
        static SsidManager instance;
        return instance;
    }

    void AddSsid(const std::string& ssid, const std::string& password);
    void RemoveSsid(int index);
    void SetDefaultSsid(int index);
    void Clear();
    std::vector<SsidItem> GetSsidList() const;

private:
    SsidManager();
    ~SsidManager();

    void LoadFromNvs();
    void SaveToNvs();

    mutable std::mutex mutex_;
    std::vector<SsidItem> ssid_list_;
};

#endif // SSID_MANAGER_H
