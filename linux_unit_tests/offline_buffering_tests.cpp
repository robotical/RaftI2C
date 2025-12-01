/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Offline buffering host-side integration tests
//
// Validates selection gating, pause/resume, backlog flush, and global RAM guard logic.
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>

#include "freertos_shim.h"
#include "DeviceIdentMgr.h"
#include "BusStatusMgr.h"
#include "DeviceTypeRecords.h"
#include "DeviceTypeRecordDynamic.h"
#include "BusAddrStatus.h"
#include "DeviceStatus.h"
#include "RaftJson.h"

class DummyBus : public RaftBus
{
public:
    DummyBus() : RaftBus(nullptr, nullptr) {}
    String getBusName() const override { return "I2C"; }
};

struct DeviceContext
{
    BusElemAddrType addr = 0;
    DeviceStatus status;
    uint16_t deviceTypeIdx = 0;
    uint32_t pollPayloadSize = 0;
};

static BusReqSyncFn dummyBusReqFn = [](const BusRequestInfo* pReqRec, std::vector<uint8_t>* pReadData) {
    if (pReadData && pReqRec)
        pReadData->assign(pReqRec->getReadReqLen(), 0);
    return RAFT_OK;
};

static DeviceTypeRecordDynamic makeDynDeviceType(const std::string& name, BusElemAddrType addr, uint32_t payloadSize)
{
    // payloadSize includes the timestamp bytes
    const uint32_t tsBytes = DevicePollingInfo::POLL_RESULT_TIMESTAMP_SIZE;
    const uint32_t readBytes = (payloadSize > tsBytes) ? payloadSize - tsBytes : 0;

    std::stringstream addrStr;
    addrStr << "0x" << std::hex << addr;

    std::stringstream pollInfo;
    pollInfo << "{\"c\":\"0x00=r" << readBytes << "\",\"i\":100,\"s\":8}";

    std::stringstream devInfo;
    devInfo << "{\"name\":\"" << name << "\"}";

    return DeviceTypeRecordDynamic(
        name.c_str(),
        addrStr.str().c_str(),
        "",
        "",
        pollInfo.str().c_str(),
        readBytes,
        devInfo.str().c_str(),
        nullptr);
}

static DeviceContext setupDevice(DeviceIdentMgr& identMgr, BusStatusMgr& statusMgr,
            const DeviceTypeRecordDynamic& dynType, BusElemAddrType addr)
{
    DeviceContext ctx;
    ctx.addr = addr;
    deviceTypeRecords.addExtendedDeviceTypeRecord(dynType, ctx.deviceTypeIdx);

    bool isOnline = false;
    statusMgr.updateBusElemState(addr, true, isOnline);

    identMgr.identifyDevice(addr, ctx.status);
    statusMgr.setBusElemDeviceStatus(addr, ctx.status);
    ctx.pollPayloadSize = ctx.status.deviceIdentPolling.pollResultSizeIncTimestamp;
    return ctx;
}

static std::vector<uint8_t> makePayload(uint32_t payloadSize, uint16_t tsVal, uint8_t fillBase)
{
    std::vector<uint8_t> data(payloadSize, fillBase);
    if (payloadSize >= 2)
    {
        data[0] = (tsVal >> 8) & 0xff;
        data[1] = tsVal & 0xff;
    }
    for (uint32_t i = 2; i < payloadSize; i++)
        data[i] = fillBase + static_cast<uint8_t>(i);
    return data;
}

static bool testPauseResumeAndFlush()
{
    std::vector<std::string> failures;
    DummyBus bus;
    BusStatusMgr statusMgr(bus);
    BusStatusMgr& statusRef = statusMgr;
    BusElemStatusCB statusCb = [](RaftBus&, const std::vector<BusElemAddrAndStatus>&) {};
    BusOperationStatusCB opCb = [](RaftBus&, BusOperationStatus) {};
    (void)statusCb;
    (void)opCb;

    RaftJson cfg("{\"offlineBuffer\":{\"perDeviceMaxBytes\":4096,\"globalMaxBytes\":8192,\"defaultWindowMs\":1000,\"maxPerPublish\":8}}");
    statusMgr.setup(cfg);
    DeviceIdentMgr identMgr(statusRef, dummyBusReqFn);
    identMgr.setup(cfg);

    DeviceContext dev = setupDevice(identMgr, statusMgr, makeDynDeviceType("PauseType", 0x31, 6), 0x31);

    identMgr.setOfflineBufferPaused({dev.addr}, true);
    statusMgr.handlePollResult(0, 1000, dev.addr, makePayload(dev.pollPayloadSize, 1, 0x11), nullptr, 0);
    if (statusMgr.getOfflineStats(dev.addr).depth != 0)
        failures.push_back("Buffer pause did not block writes");

    identMgr.setOfflineBufferPaused({dev.addr}, false);
    statusMgr.handlePollResult(0, 2000, dev.addr, makePayload(dev.pollPayloadSize, 2, 0x22), nullptr, 0);
    if (statusMgr.getOfflineStats(dev.addr).depth != 1)
        failures.push_back("Buffer resume did not capture entry");

    identMgr.setOfflineDrainPaused({dev.addr}, true);
    statusMgr.handlePollResult(0, 3000, dev.addr, makePayload(dev.pollPayloadSize, 3, 0x33), nullptr, 0);
    if (statusMgr.getOfflineStats(dev.addr).depth != 2)
        failures.push_back("Drain pause did not keep backlog");

    // Drain paused: queued data should remain intact
    identMgr.getQueuedDeviceDataJson();
    if (statusMgr.getOfflineStats(dev.addr).depth != 2)
        failures.push_back("Drain pause allowed backlog to flush");

    identMgr.setOfflineDrainPaused({dev.addr}, false);
    String drained = identMgr.getQueuedDeviceDataJson();
    if (statusMgr.getOfflineStats(dev.addr).depth != 0)
        failures.push_back("Backlog not flushed on resume");
    if (drained.indexOf("_buf") < 0)
        failures.push_back("Backlog marker missing after flush");

    if (!failures.empty())
    {
        std::cout << "[FAIL] Pause/Resume: " << failures.front() << std::endl;
        return false;
    }
    std::cout << "[PASS] Pause/Resume + reconnect flush" << std::endl;
    return true;
}

static bool testSelectionGate()
{
    std::vector<std::string> failures;
    DummyBus bus;
    BusStatusMgr statusMgr(bus);
    RaftJson cfg("{\"offlineBuffer\":{\"perDeviceMaxBytes\":4096,\"globalMaxBytes\":8192,\"defaultWindowMs\":1000,\"maxPerPublish\":8}}");
    statusMgr.setup(cfg);
    DeviceIdentMgr identMgr(statusMgr, dummyBusReqFn);
    identMgr.setup(cfg);

    DeviceContext selA = setupDevice(identMgr, statusMgr, makeDynDeviceType("SelTypeA", 0x32, 6), 0x32);
    DeviceContext selB = setupDevice(identMgr, statusMgr, makeDynDeviceType("SelTypeB", 0x33, 6), 0x33);

    statusMgr.handlePollResult(0, 4000, selA.addr, makePayload(selA.pollPayloadSize, 4, 0x44), nullptr, 0);
    statusMgr.handlePollResult(0, 5000, selB.addr, makePayload(selB.pollPayloadSize, 5, 0x55), nullptr, 0);

    identMgr.setOfflineDrainSelection({selA.addr}, {}, true);
    identMgr.getQueuedDeviceDataJson();

    if (statusMgr.getOfflineStats(selA.addr).depth != 0)
        failures.push_back("Selected address backlog not drained");
    if (statusMgr.getOfflineStats(selB.addr).depth == 0)
        failures.push_back("Unselected address drained unexpectedly");

    if (!failures.empty())
    {
        std::cout << "[FAIL] Selection gate: " << failures.front() << std::endl;
        return false;
    }
    std::cout << "[PASS] Selection gate drains only chosen devices" << std::endl;
    return true;
}

static uint32_t expectedWindowDepth(uint32_t windowMs, uint32_t intervalMs, uint32_t minSamples)
{
    uint32_t desired = intervalMs ? (windowMs + intervalMs - 1) / intervalMs : minSamples;
    return desired < minSamples ? minSamples : desired;
}

static bool testGlobalRamGuard()
{
    std::vector<std::string> failures;
    const uint32_t pollPayloadSize = 6;
    const uint32_t intervalMs = 100;
    const uint32_t windowMs = 1000;
    const uint32_t minSamples = 4;
    const uint32_t entryBytes = pollPayloadSize + OfflineDataStore::META_STORAGE_BYTES;
    const uint32_t globalMaxBytes = entryBytes * 12;

    std::stringstream cfgStr;
    cfgStr << "{\"offlineBuffer\":{\"perDeviceMaxBytes\":" << entryBytes * 20
           << ",\"globalMaxBytes\":" << globalMaxBytes
           << ",\"defaultWindowMs\":" << windowMs
           << ",\"minSamples\":" << minSamples
           << ",\"maxPerPublish\":8}}";

    DummyBus bus;
    BusStatusMgr statusMgr(bus);
    RaftJson cfg(cfgStr.str().c_str());
    statusMgr.setup(cfg);
    DeviceIdentMgr identMgr(statusMgr, dummyBusReqFn);
    identMgr.setup(cfg);

    DeviceContext devA = setupDevice(identMgr, statusMgr, makeDynDeviceType("GuardTypeA", 0x34, pollPayloadSize), 0x34);
    OfflineDataStats statsA = statusMgr.getOfflineStats(devA.addr);
    uint32_t expectedDepthA = expectedWindowDepth(windowMs, intervalMs, minSamples);
    if (statsA.maxEntries != expectedDepthA)
        failures.push_back("Primary device depth unexpected");
    uint32_t allocBytesA = statsA.maxEntries * (statsA.payloadSize + OfflineDataStore::META_STORAGE_BYTES);

    DeviceContext devB = setupDevice(identMgr, statusMgr, makeDynDeviceType("GuardTypeB", 0x35, pollPayloadSize), 0x35);
    OfflineDataStats statsB = statusMgr.getOfflineStats(devB.addr);
    uint32_t remainingEntries = (globalMaxBytes > allocBytesA) ?
            (globalMaxBytes - allocBytesA) / (statsB.payloadSize + OfflineDataStore::META_STORAGE_BYTES) : 0;
    if (statsB.maxEntries != remainingEntries)
        failures.push_back("Global guard did not cap secondary depth");
    if (statusMgr.getOfflineBytesInUse() > globalMaxBytes)
        failures.push_back("Global guard exceeded configured bytes");

    if (!failures.empty())
    {
        std::cout << "[FAIL] Global RAM guard: " << failures.front() << std::endl;
        return false;
    }
    std::cout << "[PASS] Global RAM guard caps total allocation" << std::endl;
    return true;
}

int main()
{
    int failCount = 0;
    if (!testPauseResumeAndFlush())
        failCount++;
    if (!testSelectionGate())
        failCount++;
    if (!testGlobalRamGuard())
        failCount++;

    if (failCount == 0)
        std::cout << "All offline buffering tests passed\n";
    else
        std::cout << failCount << " offline buffering tests failed\n";
    return failCount == 0 ? 0 : 1;
}
