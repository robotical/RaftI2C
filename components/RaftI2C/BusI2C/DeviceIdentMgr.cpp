/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Device Ident Manager
//
// Rob Dobson 2024
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "DeviceIdentMgr.h"
#include "DeviceTypeRecords.h"
#include "BusRequestInfo.h"
#include "RaftDevice.h"
#include "BusI2CAddrAndSlot.h"
#include "Logger.h"
#include "RaftJsonPrefixed.h"
#include "OfflineDataStore.h"
#include "RaftUtils.h"
#include <algorithm>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

// Info
#define INFO_NEW_DEVICE_IDENTIFIED

// Debug
// #define DEBUG_DEVICE_IDENT_MGR
// #define DEBUG_DEVICE_IDENT_MGR_DETAIL
// #define DEBUG_HANDLE_BUS_DEVICE_INFO
// #define DEBUG_GET_DECODED_POLL_RESPONSES

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Consructor
///////////////////////////////////////////////////////////////////////////////////////////////////////////////

DeviceIdentMgr::DeviceIdentMgr(BusStatusMgr& BusStatusMgr, BusReqSyncFn busReqSyncFn) :
    _busStatusMgr(BusStatusMgr),
    _busReqSyncFn(busReqSyncFn)
{
    _offlineCtrlMutex = xSemaphoreCreateMutex();
    _globalBufferPaused = true;
    _globalDrainPaused = true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Setup
///////////////////////////////////////////////////////////////////////////////////////////////////////////////

void DeviceIdentMgr::setup(const RaftJsonIF& config)
{
    // Enabled
    _isEnabled = config.getBool("identEnable", true);
    parseOfflineConfig(config);
    loadOfflineResumeState(config);

    // Debug
    LOG_I(MODULE_PREFIX, "DeviceIdentMgr setup %s offlineBuf windowMs %d perDevBytes %d globalBytes %d maxPerPub %d", 
                _isEnabled ? "enabled" : "disabled",
                _offlinePolicy.defaultWindowMs, _offlinePolicy.perDeviceMaxBytes, _offlinePolicy.globalMaxBytes,
                _offlinePolicy.maxPerPublish);
    if (_offlineNvsConfig.enabled)
    {
        LOG_I(MODULE_PREFIX, "offlineBuf NVS mirror flushMs %u importOnBoot %s",
                (unsigned)_offlineNvsConfig.flushIntervalMs,
                _offlineNvsConfig.importOnBoot ? "Y" : "N");
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Parse offline buffer config from bus config
void DeviceIdentMgr::parseOfflineConfig(const RaftJsonIF& config)
{
    RaftJsonPrefixed offlineCfg(config, "offlineBuffer");
    _offlinePolicy.perDeviceMaxBytes = offlineCfg.getLong("perDeviceMaxBytes", 2048);
    _offlinePolicy.globalMaxBytes = offlineCfg.getLong("globalMaxBytes", 16384);
    _offlinePolicy.defaultWindowMs = offlineCfg.getLong("defaultWindowMs", 10000);
    _offlinePolicy.minSamples = offlineCfg.getLong("minSamples", 4);
    _offlinePolicy.maxPerPublish = offlineCfg.getLong("maxPerPublish", 32);
    int32_t memUsePercent = offlineCfg.getLong("memUsePercent", -1);
    if (memUsePercent >= 0)
    {
        uint32_t permille = (uint32_t)memUsePercent * 10;
        if (permille > 1000)
            permille = 1000;
        _offlinePolicy.memUsePermille = permille;
    }
    _offlinePolicy.memUsePermille = offlineCfg.getLong("memUsePermille", _offlinePolicy.memUsePermille);

    // NVS mirror settings
    String storageMode = offlineCfg.getString("storage", "ram");
    storageMode.toLowerCase();
    _offlineNvsConfig.enabled = storageMode.indexOf("nvs") >= 0;
    uint32_t flushMs = offlineCfg.getLong("flushIntervalMs", 10000);
    if (flushMs < 1000)
        flushMs = 1000;
    if (flushMs > 600000)
        flushMs = 600000;
    _offlineNvsConfig.flushIntervalMs = flushMs;
    _offlineNvsConfig.importOnBoot = offlineCfg.getBool("importOnBoot", true);

    // Per device overrides
    std::vector<String> devOverrides;
    if (offlineCfg.getArrayElems("devices", devOverrides))
    {
        for (const String& devJson : devOverrides)
        {
            RaftJson devCfg(devJson);
            String typeName = devCfg.getString("type", "");
            uint32_t windowMs = devCfg.getLong("windowMs", 0);
            if ((typeName.length() > 0) && (windowMs > 0))
            {
                _offlinePolicy.perDeviceWindowMs[std::string(typeName.c_str())] = windowMs;
            }
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Load auto-resume state from NVS (if enabled)
void DeviceIdentMgr::loadOfflineResumeState(const RaftJsonIF& config)
{
    if (_offlineResumeLoaded)
        return;
    _offlineResumeLoaded = true;

    _offlineResume.active = false;
    _offlineResume.rateOverrideMs = 0;
    _offlineResume.targetAddrs.clear();
    _offlineResumePending.clear();

    if (!_offlineNvsConfig.enabled)
        return;

    String busName = config.getString("name", "");
    String ns = "obres";
    if (busName.length() > 0)
    {
        ns += "_";
        ns += busName;
    }
    if (ns.length() > 15)
        ns = ns.substring(0, 15);
    _offlineResumeNamespace = ns;

#ifdef ESP_PLATFORM
    _offlineResumeNvs.reset(new RaftJsonNVS(_offlineResumeNamespace.c_str()));
#else
    _offlineResumeNvs.reset(new RaftJsonNVS());
#endif
    if (!_offlineResumeNvs)
        return;

    bool active = _offlineResumeNvs->getBool("active", false);
    uint32_t rateMs = _offlineResumeNvs->getLong("rateMs", 0);
    std::vector<String> addrElems;
    if (_offlineResumeNvs->getArrayElems("addrs", addrElems))
    {
        for (const auto& elem : addrElems)
        {
            String token = elem;
            token.trim();
            if (token.length() == 0)
                continue;
            BusElemAddrType addr = strtoul(token.c_str(), nullptr, 0);
            if (addr != 0)
                _offlineResume.targetAddrs.insert(addr);
        }
    }

    if (rateMs < 10)
        rateMs = 0;
    if (rateMs > 60000)
        rateMs = 60000;

    _offlineResume.active = active && !_offlineResume.targetAddrs.empty();
    _offlineResume.rateOverrideMs = _offlineResume.active ? rateMs : 0;

    if (_offlineResume.active)
    {
        if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
        {
            _globalBufferPaused = false;
            _globalDrainPaused = true;
            xSemaphoreGive(_offlineCtrlMutex);
        }
        LOG_I(MODULE_PREFIX, "offline auto-resume loaded targets %u rateMs %u",
                (unsigned)_offlineResume.targetAddrs.size(), (unsigned)_offlineResume.rateOverrideMs);
    }
    LOG_I(MODULE_PREFIX, "offline auto-resume state active %s targets %u rateMs %u",
            _offlineResume.active ? "Y" : "N",
            (unsigned)_offlineResume.targetAddrs.size(),
            (unsigned)_offlineResume.rateOverrideMs);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Persist auto-resume state to NVS
void DeviceIdentMgr::saveOfflineResumeState()
{
    if (!_offlineResumeLoaded || !_offlineNvsConfig.enabled || !_offlineResumeNvs)
        return;

    String json = "{";
    json += "\"active\":";
    json += _offlineResume.active ? "1" : "0";
    json += ",\"rateMs\":";
    json += String(_offlineResume.rateOverrideMs);
    json += ",\"addrs\":[";
    bool first = true;
    for (auto addr : _offlineResume.targetAddrs)
    {
        if (!first)
            json += ",";
        json += "\"0x";
        json += String(addr, 16);
        json += "\"";
        first = false;
    }
    json += "]}";

    _offlineResumeNvs->setJsonDoc(json.c_str());
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Loop (periodic service)
///////////////////////////////////////////////////////////////////////////////////////////////////////////////

void DeviceIdentMgr::loop()
{
    processOfflineResumePending();
    if (!_offlineNvsConfig.enabled)
        return;
    if (_offlineNvsStates.empty())
    {
        static bool loggedNoNvsStates = false;
        if (!loggedNoNvsStates)
        {
            LOG_I(MODULE_PREFIX, "offline NVS loop skip (no configured devices yet)");
            loggedNoNvsStates = true;
        }
        return;
    }
    uint32_t nowMs = millis();
    flushOfflineNvs(nowMs);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Configure NVS mirror for an address (if enabled)
void DeviceIdentMgr::configureOfflineNvsState(BusElemAddrType address, const DevicePollingInfo& pollInfo, uint32_t maxEntries)
{
    if (!_offlineNvsConfig.enabled || maxEntries == 0 || pollInfo.pollResultSizeIncTimestamp == 0)
        return;

    LOG_I(MODULE_PREFIX, "offline NVS config addr %s maxEntries %u payload %u",
            BusI2CAddrAndSlot::toString(address).c_str(), (unsigned)maxEntries,
            (unsigned)pollInfo.pollResultSizeIncTimestamp);

    OfflineNvsState& state = _offlineNvsStates[address];
    state.payloadSize = pollInfo.pollResultSizeIncTimestamp;
    state.timestampBytes = DevicePollingInfo::POLL_RESULT_TIMESTAMP_SIZE;
    state.timestampResolutionUs = DevicePollingInfo::POLL_RESULT_RESOLUTION_US;
    state.ramMaxEntries = maxEntries;

    char nsBuf[16];
    snprintf(nsBuf, sizeof(nsBuf), "ob%08x", (unsigned)address);
    if (!state.store.configure(nsBuf, state.payloadSize, state.timestampBytes, state.timestampResolutionUs, maxEntries))
    {
        LOG_W(MODULE_PREFIX, "offline NVS configure failed addr %s",
                BusI2CAddrAndSlot::toString(address).c_str());
        return;
    }
    state.store.setEffectiveMaxEntries(maxEntries);
    LOG_I(MODULE_PREFIX, "offline NVS ready addr %s count %u nextSeq %u imported %s",
            BusI2CAddrAndSlot::toString(address).c_str(),
            (unsigned)state.store.getCount(), (unsigned)state.store.getNextSeq(),
            state.imported ? "Y" : "N");

    if (!state.hasFlushedSeq)
    {
        uint32_t nextSeq = state.store.getNextSeq();
        if (state.store.getCount() > 0 && nextSeq > 0)
        {
            state.lastFlushedSeq = nextSeq - 1;
            state.hasFlushedSeq = true;
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Import NVS mirror into RAM once, after buffer config is settled
void DeviceIdentMgr::importOfflineNvsIfNeeded(BusElemAddrType address, uint32_t maxEntries)
{
    if (!_offlineNvsConfig.enabled || !_offlineNvsConfig.importOnBoot)
        return;

    auto it = _offlineNvsStates.find(address);
    if (it == _offlineNvsStates.end())
        return;

    OfflineNvsState& state = it->second;
    if (state.imported || !state.store.isReady())
        return;

    uint32_t importMax = maxEntries > 0 ? maxEntries : state.ramMaxEntries;
    if (importMax == 0)
        return;

    uint32_t nextSeq = 0;
    LOG_I(MODULE_PREFIX, "offline NVS import start addr %s count %u",
            BusI2CAddrAndSlot::toString(address).c_str(), (unsigned)state.store.getCount());
    bool imported = _busStatusMgr.importOfflineFromNVS(address, state.store, importMax, nextSeq);
    if (imported)
    {
        state.imported = true;
        if (nextSeq > 0)
        {
            state.lastFlushedSeq = nextSeq - 1;
            state.hasFlushedSeq = true;
        }
    }
    else if (state.store.getCount() == 0)
    {
        state.imported = true;
    }
    LOG_I(MODULE_PREFIX, "offline NVS import done addr %s imported %s nextSeq %u",
            BusI2CAddrAndSlot::toString(address).c_str(), imported ? "Y" : "N",
            (unsigned)nextSeq);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Flush offline buffers to NVS as needed
void DeviceIdentMgr::flushOfflineNvs(uint32_t nowMs)
{
    static const uint32_t NVS_FLUSH_CHUNK_BYTES = 64 * 1024;
    if (_offlineNvsConfig.flushIntervalMs == 0)
        return;

    for (auto& kv : _offlineNvsStates)
    {
        BusElemAddrType address = kv.first;
        OfflineNvsState& state = kv.second;
        if (!state.store.isReady() || state.ramMaxEntries == 0)
            continue;
        if (!Raft::isTimeout(nowMs, state.lastFlushMs, _offlineNvsConfig.flushIntervalMs))
            continue;

        OfflineDataStats stats = _busStatusMgr.getOfflineStats(address);
        LOG_I(MODULE_PREFIX, "offline NVS flush check addr %s depth %u firstSeq %u max %u lastFlushed %u hasFlushed %s storeCount %u nextSeq %u",
                BusI2CAddrAndSlot::toString(address).c_str(),
                (unsigned)stats.depth, (unsigned)stats.firstSeq, (unsigned)stats.maxEntries,
                (unsigned)state.lastFlushedSeq, state.hasFlushedSeq ? "Y" : "N",
                (unsigned)state.store.getCount(), (unsigned)state.store.getNextSeq());
        if (stats.maxEntries == 0 || stats.payloadSize == 0)
        {
            state.lastFlushMs = nowMs;
            continue;
        }
        if (stats.depth == 0)
        {
            if (state.lastFlushMs == 0)
            {
                LOG_I(MODULE_PREFIX, "offline NVS flush skip addr %s depth 0 (no data yet)",
                        BusI2CAddrAndSlot::toString(address).c_str());
            }
            state.lastFlushMs = nowMs;
            continue;
        }

        state.store.setEffectiveMaxEntries(state.ramMaxEntries);

        uint32_t startIdx = 0;
        if (state.hasFlushedSeq)
        {
            if (state.lastFlushedSeq >= stats.firstSeq)
            {
                startIdx = state.lastFlushedSeq - stats.firstSeq + 1;
            }
            else
            {
                startIdx = 0;
            }
        }
        if (startIdx >= stats.depth)
        {
            LOG_I(MODULE_PREFIX, "offline NVS flush skip addr %s no new entries depth %u firstSeq %u lastSeq %u",
                    BusI2CAddrAndSlot::toString(address).c_str(), (unsigned)stats.depth,
                    (unsigned)stats.firstSeq, (unsigned)state.lastFlushedSeq);
            state.lastFlushMs = nowMs;
            continue;
        }

        LOG_I(MODULE_PREFIX, "offline NVS flush addr %s depth %u firstSeq %u startIdx %u lastSeq %u",
                BusI2CAddrAndSlot::toString(address).c_str(), (unsigned)stats.depth,
                (unsigned)stats.firstSeq, (unsigned)startIdx, (unsigned)state.lastFlushedSeq);

        uint32_t tsResMs = stats.timestampResolutionUs / 1000;
        while (startIdx < stats.depth)
        {
            bool isOnline = false;
            uint16_t deviceTypeIndex = 0;
            std::vector<uint8_t> devicePollResponseData;
            uint32_t responseSize = 0;
            std::vector<OfflineDataMeta> metas;
            OfflineDataStats statsNow;
            uint32_t numResponses = _busStatusMgr.peekBusElemOfflineResponses(address, isOnline, deviceTypeIndex,
                        devicePollResponseData, responseSize, startIdx, 0, NVS_FLUSH_CHUNK_BYTES, metas, statsNow);
            if (numResponses == 0 || metas.empty())
                break;

            std::vector<uint32_t> adjTsMs;
            adjTsMs.reserve(numResponses);
            for (uint32_t ii = 0; ii < numResponses; ii++)
            {
                uint64_t adj = metas[ii].tsBaseMs + (uint64_t)metas[ii].ts * tsResMs;
                adjTsMs.push_back((uint32_t)adj);
            }

            uint32_t lastSeq = 0;
            bool ok = state.store.appendBatch(devicePollResponseData, responseSize, adjTsMs,
                        metas.front().seq, numResponses, lastSeq);
            if (!ok)
            {
                LOG_W(MODULE_PREFIX, "offline NVS append failed addr %s responses %u",
                        BusI2CAddrAndSlot::toString(address).c_str(), (unsigned)numResponses);
                break;
            }
            LOG_I(MODULE_PREFIX, "offline NVS append addr %s responses %u lastSeq %u",
                    BusI2CAddrAndSlot::toString(address).c_str(), (unsigned)numResponses, (unsigned)lastSeq);
            state.lastFlushedSeq = lastSeq;
            state.hasFlushedSeq = true;
            startIdx += numResponses;
        }

        state.lastFlushMs = nowMs;
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Clear NVS mirror state for addresses
void DeviceIdentMgr::clearOfflineNvsState(const std::vector<BusElemAddrType>& addresses)
{
    if (!_offlineNvsConfig.enabled)
        return;
    for (auto addr : addresses)
    {
        auto it = _offlineNvsStates.find(addr);
        if (it == _offlineNvsStates.end())
            continue;
        it->second.store.clear();
        it->second.imported = false;
        it->second.hasFlushedSeq = false;
        it->second.lastFlushedSeq = 0;
        it->second.lastFlushMs = 0;
        it->second.ramMaxEntries = 0;
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Ensure NVS mirror is configured (and imported) for peeked addresses
void DeviceIdentMgr::ensureOfflineNvsForPeek(const std::vector<BusElemAddrType>& addresses)
{
    if (!_offlineNvsConfig.enabled || !_offlineNvsConfig.importOnBoot)
        return;

    std::vector<BusElemAddrType> targetAddrs = addresses;
    if (targetAddrs.empty())
        _busStatusMgr.getBusElemAddresses(targetAddrs, false);

    for (auto addr : targetAddrs)
    {
        DevicePollingInfo pollInfo;
        if (!_busStatusMgr.getDevicePollingInfo(addr, pollInfo))
            continue;

        OfflineDataStats stats = _busStatusMgr.getOfflineStats(addr);
        uint32_t depth = stats.maxEntries;
        if (depth == 0)
        {
            depth = computeDepthForAddress(addr, pollInfo);
            if (depth == 0)
                continue;
            _busStatusMgr.reconfigureOfflineBuffer(addr, depth,
                    pollInfo.pollResultSizeIncTimestamp,
                    DevicePollingInfo::POLL_RESULT_TIMESTAMP_SIZE,
                    DevicePollingInfo::POLL_RESULT_RESOLUTION_US);
            LOG_I(MODULE_PREFIX, "offline NVS peek configure addr %s depth %u payload %u",
                    BusI2CAddrAndSlot::toString(addr).c_str(), (unsigned)depth,
                    (unsigned)pollInfo.pollResultSizeIncTimestamp);
        }
        configureOfflineNvsState(addr, pollInfo, depth);
        importOfflineNvsIfNeeded(addr, depth);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Queue auto-resume for a newly identified address
void DeviceIdentMgr::queueOfflineResume(BusElemAddrType address)
{
    if (!_offlineResume.active)
        return;

    bool shouldResume = false;
    if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
    {
        _bufferPausedAddrs.insert(address);
        _drainPausedAddrs.insert(address);
        shouldResume = (_offlineResume.targetAddrs.count(address) > 0);
        if (shouldResume)
            _offlineResumePending.insert(address);
        xSemaphoreGive(_offlineCtrlMutex);
    }
    else
    {
        _bufferPausedAddrs.insert(address);
        _drainPausedAddrs.insert(address);
        shouldResume = (_offlineResume.targetAddrs.count(address) > 0);
        if (shouldResume)
            _offlineResumePending.insert(address);
    }

    if (shouldResume)
    {
        LOG_I(MODULE_PREFIX, "offline auto-resume queued addr %s",
                BusI2CAddrAndSlot::toString(address).c_str());
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Process any pending auto-resume requests
void DeviceIdentMgr::processOfflineResumePending()
{
    if (!_offlineResume.active || _offlineResumePending.empty())
        return;

    std::vector<BusElemAddrType> pendingAddrs;
    if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
    {
        pendingAddrs.assign(_offlineResumePending.begin(), _offlineResumePending.end());
        _offlineResumePending.clear();
        xSemaphoreGive(_offlineCtrlMutex);
    }
    else
    {
        return;
    }

    for (auto addr : pendingAddrs)
    {
        DevicePollingInfo pollInfo;
        if (!_busStatusMgr.getDevicePollingInfo(addr, pollInfo))
            continue;

        OfflineDataStats stats = _busStatusMgr.getOfflineStats(addr);
        uint32_t depth = stats.maxEntries;
        if (depth == 0)
        {
            depth = computeDepthForAddress(addr, pollInfo);
            if (depth == 0)
                continue;
            _busStatusMgr.reconfigureOfflineBuffer(addr, depth,
                    pollInfo.pollResultSizeIncTimestamp,
                    DevicePollingInfo::POLL_RESULT_TIMESTAMP_SIZE,
                    DevicePollingInfo::POLL_RESULT_RESOLUTION_US);
        }

        if (_offlineResume.rateOverrideMs > 0)
            applyRateOverrideToAddress(addr, _offlineResume.rateOverrideMs, true);

        if (_offlineNvsConfig.enabled)
        {
            OfflineDataStats statsNow = _busStatusMgr.getOfflineStats(addr);
            configureOfflineNvsState(addr, pollInfo, statsNow.maxEntries > 0 ? statsNow.maxEntries : depth);
            importOfflineNvsIfNeeded(addr, statsNow.maxEntries);
        }

        if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
        {
            _bufferPausedAddrs.erase(addr);
            xSemaphoreGive(_offlineCtrlMutex);
        }
        _busStatusMgr.setOfflineBufferPaused(addr, false);
        _busStatusMgr.setOfflineDrainPaused(addr, true);

        LOG_I(MODULE_PREFIX, "offline auto-resume start addr %s depth %u rateMs %u",
                BusI2CAddrAndSlot::toString(addr).c_str(), (unsigned)depth,
                (unsigned)_offlineResume.rateOverrideMs);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Calculate offline depth based on config and poll interval
uint32_t DeviceIdentMgr::calcOfflineDepth(const DeviceTypeRecord& devTypeRec, const DevicePollingInfo& pollInfo) const
{
    uint32_t windowMs = _offlinePolicy.defaultWindowMs;
    std::string devTypeName = devTypeRec.deviceType ? devTypeRec.deviceType : "";
    auto windowIt = _offlinePolicy.perDeviceWindowMs.find(devTypeName);
    if (windowIt != _offlinePolicy.perDeviceWindowMs.end())
        windowMs = windowIt->second;

    uint32_t intervalMs = pollInfo.pollIntervalUs / 1000;
    uint32_t bytesPerEntry = pollInfo.pollResultSizeIncTimestamp + OfflineDataStore::META_STORAGE_BYTES;
    uint32_t desired = (intervalMs > 0) ? (windowMs + intervalMs - 1) / intervalMs : _offlinePolicy.minSamples;
    if (desired < _offlinePolicy.minSamples)
        desired = _offlinePolicy.minSamples;

    if (_offlinePolicy.perDeviceMaxBytes > 0 && bytesPerEntry > 0 && (_offlinePolicy.memUsePermille == 0))
    {
        uint32_t maxFromBytes = _offlinePolicy.perDeviceMaxBytes / bytesPerEntry;
        if (maxFromBytes == 0)
            desired = 1;
        else if (maxFromBytes < desired)
            desired = maxFromBytes;
    }

    // Try to scale up to use a share of available memory (75% default) across devices
#ifdef ESP_PLATFORM
    const uint32_t BUDGET_HEADROOM_BYTES = 1024 * 1024;
    uint32_t currentBytes = _busStatusMgr.getOfflineBytesInUse();
    uint64_t freeMem = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (freeMem == 0)
        freeMem = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint64_t totalMemForBudget = freeMem + currentBytes;
    if (totalMemForBudget > 0 && bytesPerEntry > 0)
    {
        uint64_t budgetBytes = currentBytes;
        if (_offlinePolicy.memUsePermille > 0)
            budgetBytes += (freeMem * _offlinePolicy.memUsePermille) / 1000;
        else
            budgetBytes += freeMem;
        if ((_offlinePolicy.globalMaxBytes > 0) && (budgetBytes > _offlinePolicy.globalMaxBytes))
            budgetBytes = _offlinePolicy.globalMaxBytes;
        budgetBytes = (budgetBytes > BUDGET_HEADROOM_BYTES) ? (budgetBytes - BUDGET_HEADROOM_BYTES) : 0;

        uint32_t numDevices = 1;
        std::vector<BusElemAddrType> addrList;
        _busStatusMgr.getBusElemAddresses(addrList, false);
        if (!addrList.empty())
            numDevices = addrList.size();

        uint64_t perDeviceBudget = budgetBytes / numDevices;
        uint32_t maxFromMem = perDeviceBudget / bytesPerEntry;
        if (maxFromMem > desired)
            desired = maxFromMem;
        LOG_I(MODULE_PREFIX, "calcOfflineDepth freeMem %u currentBytes %u budget %u perDev %u bytesPerEntry %u desired %u",
                (unsigned)freeMem, (unsigned)currentBytes, (unsigned)budgetBytes, (unsigned)perDeviceBudget,
                (unsigned)bytesPerEntry, (unsigned)desired);
    }
#endif

    return desired == 0 ? 1 : desired;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Apply global offline buffer limit
uint32_t DeviceIdentMgr::applyGlobalOfflineLimit(const DevicePollingInfo& pollInfo, uint32_t requestedDepth) const
{
    const uint32_t BUDGET_HEADROOM_BYTES = 1024 * 1024;
    if (requestedDepth == 0)
        return 0;

    uint32_t bytesPerEntry = pollInfo.pollResultSizeIncTimestamp + OfflineDataStore::META_STORAGE_BYTES;
    if (bytesPerEntry == 0)
        return requestedDepth;

    // Current offline allocation already reserved
    // Determine available memory to use (prefer PSRAM, fall back to internal)
    uint64_t freeMem = 0;
#ifdef ESP_PLATFORM
    freeMem = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (freeMem == 0)
        freeMem = heap_caps_get_free_size(MALLOC_CAP_8BIT);
#endif

    // Apply safety fraction (permille) and optional global cap if configured
    uint32_t currentBytes = _busStatusMgr.getOfflineBytesInUse();
    uint32_t permille = _offlinePolicy.memUsePermille;
    uint64_t budgetBytes = currentBytes;
    if (permille > 0)
        budgetBytes += (freeMem * permille) / 1000;
    else
        budgetBytes += freeMem;
    if ((_offlinePolicy.globalMaxBytes > 0) && (budgetBytes > _offlinePolicy.globalMaxBytes))
        budgetBytes = _offlinePolicy.globalMaxBytes;

    // Keep some headroom so comms etc don't starve
    budgetBytes = (budgetBytes > BUDGET_HEADROOM_BYTES) ? (budgetBytes - BUDGET_HEADROOM_BYTES) : 0;

    // Split remaining budget equally across devices
    uint64_t availableBytesTotal = budgetBytes;
    uint32_t numDevices = 1;
    {
        std::vector<BusElemAddrType> addrList;
        _busStatusMgr.getBusElemAddresses(addrList, false);
        if (!addrList.empty())
            numDevices = addrList.size();
    }
    uint64_t perDeviceBudget = availableBytesTotal / numDevices;
    if ((_offlinePolicy.perDeviceMaxBytes > 0) && (perDeviceBudget > _offlinePolicy.perDeviceMaxBytes))
        perDeviceBudget = _offlinePolicy.perDeviceMaxBytes;

    uint32_t maxDepthFromMem = perDeviceBudget / bytesPerEntry;
    if (maxDepthFromMem == 0)
        return 0;

    LOG_I(MODULE_PREFIX, "applyGlobalOfflineLimit freeMem %u currentBytes %u budget %u perDev %u bytesPerEntry %u req %u depth %u numDev %u",
            (unsigned)freeMem, (unsigned)currentBytes, (unsigned)budgetBytes,
            (unsigned)perDeviceBudget, (unsigned)bytesPerEntry, (unsigned)requestedDepth,
            (unsigned)maxDepthFromMem, (unsigned)numDevices);

    return (maxDepthFromMem < requestedDepth) ? maxDepthFromMem : requestedDepth;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Compute offline depth for an address using current policy
uint32_t DeviceIdentMgr::computeDepthForAddress(BusElemAddrType address, const DevicePollingInfo& pollInfo) const
{
    uint16_t deviceTypeIdx = _busStatusMgr.getDeviceTypeIndexByAddr(address);
    DeviceTypeRecord devTypeRec;
    if (!deviceTypeRecords.getDeviceInfo(deviceTypeIdx, devTypeRec))
        return 0;

    uint32_t depthReq = calcOfflineDepth(devTypeRec, pollInfo);
    uint32_t depth = applyGlobalOfflineLimit(pollInfo, depthReq);
    if (depth == 0)
        depth = 1;
    return depth;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get per-device publish limit
uint32_t DeviceIdentMgr::getPerDevicePublishLimit(uint32_t maxResponsesToReturn) const
{
    if (maxResponsesToReturn)
        return maxResponsesToReturn;
    if (_maxPerPublishOverride)
        return _maxPerPublishOverride;
    return _offlinePolicy.maxPerPublish;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Check if offline drain is allowed for address/type
bool DeviceIdentMgr::isOfflineDrainAllowed(BusElemAddrType address, uint16_t deviceTypeIndex) const
{
    std::string devTypeName;
    DeviceTypeRecord devTypeRec;
    if (deviceTypeRecords.getDeviceInfo(deviceTypeIndex, devTypeRec) && devTypeRec.deviceType)
        devTypeName = devTypeRec.deviceType;

    bool allowed = true;
    if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
    {
        bool paused = _drainPausedAddrs.count(address) > 0;
        bool restricted = _drainOnlySelected && (_drainSelectedAddrs.count(address) == 0) &&
                    (devTypeName.empty() || (_drainSelectedTypes.count(devTypeName) == 0));
        allowed = !(paused || restricted);
        xSemaphoreGive(_offlineCtrlMutex);
    }
    return allowed;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Compute offline control flags for an address/type
void DeviceIdentMgr::computeOfflineControlFlags(BusElemAddrType address, const std::string& devTypeName,
            bool& bufferPaused, bool& drainPaused, bool& restrictToSelection) const
{
    bufferPaused = _globalBufferPaused;
    drainPaused = _globalDrainPaused || _linkDrainPaused;
    restrictToSelection = false;
    if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
    {
        bufferPaused = bufferPaused || (_bufferPausedAddrs.count(address) > 0);
        drainPaused = drainPaused || (_drainPausedAddrs.count(address) > 0) || _linkDrainPaused;
        restrictToSelection = _drainOnlySelected && (_drainSelectedAddrs.count(address) == 0) &&
                (devTypeName.empty() || (_drainSelectedTypes.count(devTypeName) == 0));
        xSemaphoreGive(_offlineCtrlMutex);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Apply paused/selection controls to a device record
void DeviceIdentMgr::applyOfflineControlsToDevice(BusElemAddrType address, DeviceStatus& deviceStatus)
{
    std::string devTypeName;
    DeviceTypeRecord devTypeRec;
    if (deviceTypeRecords.getDeviceInfo(deviceStatus.getDeviceTypeIndex(), devTypeRec) && devTypeRec.deviceType)
        devTypeName = devTypeRec.deviceType;

    bool bufferPaused = false;
    bool drainPaused = false;
    bool restrictToSelection = false;
    computeOfflineControlFlags(address, devTypeName, bufferPaused, drainPaused, restrictToSelection);

    deviceStatus.setOfflineBufferPaused(bufferPaused);
    deviceStatus.setOfflineDrainPaused(drainPaused || restrictToSelection);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Helper to set remaining counter
void DeviceIdentMgr::setOfflineStatsRemaining(uint32_t remaining, uint32_t* pRemaining) const
{
    if (pRemaining)
        *pRemaining += remaining;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Apply control flags to existing device status entries
void DeviceIdentMgr::applyOfflineControlToAddress(BusElemAddrType address, uint16_t deviceTypeIdx)
{
    std::string devTypeName;
    DeviceTypeRecord devTypeRec;
    if (deviceTypeRecords.getDeviceInfo(deviceTypeIdx, devTypeRec) && devTypeRec.deviceType)
        devTypeName = devTypeRec.deviceType;
    bool bufferPaused = false;
    bool drainPaused = false;
    bool restrictToSelection = false;
    computeOfflineControlFlags(address, devTypeName, bufferPaused, drainPaused, restrictToSelection);
    _busStatusMgr.setOfflineBufferPaused(address, bufferPaused);
    _busStatusMgr.setOfflineDrainPaused(address, drainPaused || restrictToSelection);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Apply control flags to all known devices
void DeviceIdentMgr::applyOfflineControlToExisting()
{
    std::vector<BusElemAddrType> addresses;
    _busStatusMgr.getBusElemAddresses(addresses, false);
    for (auto address : addresses)
    {
        uint16_t deviceTypeIdx = _busStatusMgr.getDeviceTypeIndexByAddr(address);
        applyOfflineControlToAddress(address, deviceTypeIdx);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Apply poll rate override to a set of addresses
bool DeviceIdentMgr::applyOfflineRateOverride(const std::vector<BusElemAddrType>& addresses, uint32_t pollRateMs)
{
    if (pollRateMs == 0)
        return false;

    std::vector<BusElemAddrType> targetAddrs = addresses;
    if (targetAddrs.empty())
        _busStatusMgr.getBusElemAddresses(targetAddrs, false);

    bool anyUpdated = false;
    for (auto addr : targetAddrs)
    {
        anyUpdated |= applyRateOverrideToAddress(addr, pollRateMs, true);
    }
    return anyUpdated;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Clear poll rate overrides for a set of addresses
bool DeviceIdentMgr::clearOfflineRateOverride(const std::vector<BusElemAddrType>& addresses)
{
    std::vector<BusElemAddrType> targetAddrs = addresses;
    if (targetAddrs.empty())
        _busStatusMgr.getBusElemAddresses(targetAddrs, false);

    bool anyCleared = false;
    for (auto addr : targetAddrs)
    {
        anyCleared |= clearRateOverrideForAddress(addr);
    }
    return anyCleared;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Apply a poll rate override to a single address
bool DeviceIdentMgr::applyRateOverrideToAddress(BusElemAddrType address, uint32_t pollRateMs, bool recordOriginal)
{
    if (pollRateMs == 0)
        return false;

    uint32_t rateMsClamped = pollRateMs;
    if (rateMsClamped < 10)
        rateMsClamped = 10;
    if (rateMsClamped > 60000)
        rateMsClamped = 60000;
    uint32_t pollIntervalUs = rateMsClamped * 1000;

    DevicePollingInfo pollInfo;
    if (!_busStatusMgr.getDevicePollingInfo(address, pollInfo))
        return false;
    uint32_t originalIntervalUs = pollInfo.pollIntervalUs;

    if (recordOriginal && _offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
    {
        if (_rateOverrideOriginalUs.find(address) == _rateOverrideOriginalUs.end())
            _rateOverrideOriginalUs[address] = pollInfo.pollIntervalUs;
        _rateOverridesUs[address] = pollIntervalUs;
        xSemaphoreGive(_offlineCtrlMutex);
    }

    pollInfo.pollIntervalUs = pollIntervalUs;
    OfflineDataStats existingStats = _busStatusMgr.getOfflineStats(address);
    uint32_t depth = existingStats.maxEntries > 0 ? existingStats.maxEntries : computeDepthForAddress(address, pollInfo);
    bool updated = _busStatusMgr.setDevicePollInterval(address, pollIntervalUs);
    LOG_I(MODULE_PREFIX, "offline rate override addr %s intervalUs %u (was %u) depth %u payload %u",
                BusI2CAddrAndSlot::toString(address).c_str(), pollInfo.pollIntervalUs,
                originalIntervalUs, depth, pollInfo.pollResultSizeIncTimestamp);
    if (depth > 0)
    {
        _busStatusMgr.reconfigureOfflineBuffer(address, depth, pollInfo.pollResultSizeIncTimestamp,
                DevicePollingInfo::POLL_RESULT_TIMESTAMP_SIZE, DevicePollingInfo::POLL_RESULT_RESOLUTION_US);
        configureOfflineNvsState(address, pollInfo, depth);
    }
    return updated;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Rebalance offline buffer depths across devices (shared budget split equally)
bool DeviceIdentMgr::rebalanceOfflineBuffers(const std::vector<BusElemAddrType>& addresses)
{
    const uint32_t BUDGET_HEADROOM_BYTES = 1024 * 1024;
    const uint64_t MIN_TARGET_PER_DEVICE_BYTES = (uint64_t)5 * 1024 * 1024;
    bool explicitTargetsProvided = !addresses.empty();
    // Determine target addresses
    std::vector<BusElemAddrType> targetAddrs = addresses;
    if (targetAddrs.empty())
        _busStatusMgr.getBusElemAddresses(targetAddrs, false);
    if (targetAddrs.empty())
        return false;
    LOG_I(MODULE_PREFIX, "rebalance start reqAddrs %u targetAddrs %u",
            (unsigned)addresses.size(), (unsigned)targetAddrs.size());

    // If a subset was requested, shrink non-target buffers to minimum to free memory
    if (explicitTargetsProvided)
    {
        std::vector<BusElemAddrType> allAddrs;
        _busStatusMgr.getBusElemAddresses(allAddrs, false);
        for (auto addr : allAddrs)
        {
            if (std::find(targetAddrs.begin(), targetAddrs.end(), addr) != targetAddrs.end())
                continue;
            DevicePollingInfo pollInfo;
            if (!_busStatusMgr.getDevicePollingInfo(addr, pollInfo))
            {
                LOG_W(MODULE_PREFIX, "rebalance shrink addr %s missing pollInfo",
                        BusI2CAddrAndSlot::toString(addr).c_str());
                continue;
            }
            uint32_t bytesPerEntry = pollInfo.pollResultSizeIncTimestamp + OfflineDataStore::META_STORAGE_BYTES;
            uint32_t depth = _offlinePolicy.minSamples > 0 ? _offlinePolicy.minSamples : 1;
            bool shrinkOk = _busStatusMgr.reconfigureOfflineBuffer(addr, depth,
                    pollInfo.pollResultSizeIncTimestamp,
                    DevicePollingInfo::POLL_RESULT_TIMESTAMP_SIZE,
                    DevicePollingInfo::POLL_RESULT_RESOLUTION_US);
            LOG_I(MODULE_PREFIX, "rebalance shrink addr %s depth %u bytesPerEntry %u %s",
                    BusI2CAddrAndSlot::toString(addr).c_str(), (unsigned)depth, (unsigned)bytesPerEntry,
                    shrinkOk ? "ok" : "fail");
        }
    }

    // Compute total budget bytes (75% default of free mem, capped by globalMaxBytes)
    uint64_t freeMem = 0;
#ifdef ESP_PLATFORM
    freeMem = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (freeMem == 0)
        freeMem = heap_caps_get_free_size(MALLOC_CAP_8BIT);
#endif
    if (freeMem == 0 && _busStatusMgr.getOfflineBytesInUse() == 0)
    {
        LOG_W(MODULE_PREFIX, "rebalance skip freeMem 0 currentOffline 0 targets %u",
                (unsigned)targetAddrs.size());
        return false;
    }

    uint32_t currentBytes = 0;
    for (auto addr : targetAddrs)
    {
        OfflineDataStats stats = _busStatusMgr.getOfflineStats(addr);
        currentBytes += stats.maxEntries * (stats.payloadSize + stats.metaSize);
    }

    uint64_t budgetBytes = currentBytes;
    if (_offlinePolicy.memUsePermille > 0)
        budgetBytes += (freeMem * _offlinePolicy.memUsePermille) / 1000;
    else
        budgetBytes += freeMem;
    if ((_offlinePolicy.globalMaxBytes > 0) && (budgetBytes > _offlinePolicy.globalMaxBytes))
        budgetBytes = _offlinePolicy.globalMaxBytes;
    // Leave headroom to avoid starving other subsystems
    budgetBytes = (budgetBytes > BUDGET_HEADROOM_BYTES) ? (budgetBytes - BUDGET_HEADROOM_BYTES) : 0;
    // Allow consuming some of the reserved headroom to reach the per-device floor when explicitly targeting
    if (explicitTargetsProvided)
    {
        uint64_t minTotalBudget = MIN_TARGET_PER_DEVICE_BYTES * targetAddrs.size();
        if ((budgetBytes < minTotalBudget) && ((budgetBytes + BUDGET_HEADROOM_BYTES) >= minTotalBudget))
            budgetBytes = minTotalBudget;
    }

    // Split equally across targets, but when explicit targets provided ensure a high floor
    uint64_t perDeviceBudget = budgetBytes / targetAddrs.size();
    if (explicitTargetsProvided)
    {
        uint64_t minTotalBudget = MIN_TARGET_PER_DEVICE_BYTES * targetAddrs.size();
        if ((budgetBytes >= minTotalBudget) && (perDeviceBudget < MIN_TARGET_PER_DEVICE_BYTES))
        {
            perDeviceBudget = MIN_TARGET_PER_DEVICE_BYTES;
        }
        else if (perDeviceBudget < MIN_TARGET_PER_DEVICE_BYTES)
        {
            LOG_W(MODULE_PREFIX, "rebalance cap perDev floor due to budget budgetBytes %u targets %u minPerDev %u",
                    (unsigned)budgetBytes, (unsigned)targetAddrs.size(), (unsigned)MIN_TARGET_PER_DEVICE_BYTES);
        }
    }
    if ((_offlinePolicy.perDeviceMaxBytes > 0) && (perDeviceBudget > _offlinePolicy.perDeviceMaxBytes))
        perDeviceBudget = _offlinePolicy.perDeviceMaxBytes;

    bool anyUpdated = false;
    for (auto addr : targetAddrs)
    {
        DevicePollingInfo pollInfo;
        if (!_busStatusMgr.getDevicePollingInfo(addr, pollInfo))
        {
            LOG_W(MODULE_PREFIX, "rebalance addr %s missing pollInfo targets %u",
                    BusI2CAddrAndSlot::toString(addr).c_str(), (unsigned)targetAddrs.size());
            continue;
        }
        uint32_t bytesPerEntry = pollInfo.pollResultSizeIncTimestamp + OfflineDataStore::META_STORAGE_BYTES;
        if (bytesPerEntry == 0)
        {
            LOG_W(MODULE_PREFIX, "rebalance addr %s bytesPerEntry 0 payload %u tsBytes %u",
                    BusI2CAddrAndSlot::toString(addr).c_str(),
                    (unsigned)pollInfo.pollResultSizeIncTimestamp,
                    (unsigned)DevicePollingInfo::POLL_RESULT_TIMESTAMP_SIZE);
            continue;
        }
        OfflineDataStats stats = _busStatusMgr.getOfflineStats(addr);
        uint64_t currentAllocBytes = (uint64_t)stats.maxEntries * (stats.payloadSize + stats.metaSize);
        uint64_t targetBudget = perDeviceBudget;
        if (currentAllocBytes > targetBudget)
            targetBudget = currentAllocBytes;
#ifdef ESP_PLATFORM
        uint64_t freeMemNow = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (freeMemNow == 0)
            freeMemNow = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        uint64_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        if (largestBlock == 0)
            largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#else
        uint64_t freeMemNow = 0;
        uint64_t largestBlock = 0;
#endif
        uint32_t depth = targetBudget / bytesPerEntry;
        if (depth == 0)
            depth = _offlinePolicy.minSamples > 0 ? _offlinePolicy.minSamples : 1;
        uint32_t maxDepthFromBlocks = depth;
#ifdef ESP_PLATFORM
        if (largestBlock > 0)
        {
            uint32_t maxDepthFromRing = pollInfo.pollResultSizeIncTimestamp ?
                        (uint32_t)(largestBlock / pollInfo.pollResultSizeIncTimestamp) : depth;
            uint32_t maxDepthFromMeta = OfflineDataStore::META_STORAGE_BYTES ?
                        (uint32_t)(largestBlock / OfflineDataStore::META_STORAGE_BYTES) : depth;
            maxDepthFromBlocks = std::min(maxDepthFromRing, maxDepthFromMeta);
            uint32_t maxDepthFromFree = bytesPerEntry ?
                        (uint32_t)((largestBlock > freeMemNow ? largestBlock : freeMemNow) / bytesPerEntry) : depth;
            if (maxDepthFromFree < maxDepthFromBlocks)
                maxDepthFromBlocks = maxDepthFromFree;
        }
#endif
        if (maxDepthFromBlocks > 0 && depth > maxDepthFromBlocks)
        {
            LOG_W(MODULE_PREFIX, "rebalance clamp depth addr %s depthReq %u depthMax %u freeNow %u largestBlock %u",
                    BusI2CAddrAndSlot::toString(addr).c_str(), (unsigned)depth,
                    (unsigned)maxDepthFromBlocks, (unsigned)freeMemNow, (unsigned)largestBlock);
            depth = maxDepthFromBlocks;
        }
        bool updated = _busStatusMgr.reconfigureOfflineBuffer(addr, depth,
                pollInfo.pollResultSizeIncTimestamp,
                DevicePollingInfo::POLL_RESULT_TIMESTAMP_SIZE,
                DevicePollingInfo::POLL_RESULT_RESOLUTION_US);
        configureOfflineNvsState(addr, pollInfo, depth);
        importOfflineNvsIfNeeded(addr, depth);
        LOG_I(MODULE_PREFIX, "rebalance addr %s freeMem %u freeNow %u largest %u currentBytes %u budget %u headroom %u perDev %u bytesPerEntry %u depth %u targets %u updated %d",
                BusI2CAddrAndSlot::toString(addr).c_str(), (unsigned)freeMem, (unsigned)freeMemNow,
                (unsigned)largestBlock, (unsigned)currentBytes,
                (unsigned)budgetBytes, (unsigned)BUDGET_HEADROOM_BYTES, (unsigned)perDeviceBudget,
                (unsigned)bytesPerEntry, (unsigned)depth, (unsigned)targetAddrs.size(), (int)updated);
        anyUpdated |= updated;
    }
    return anyUpdated;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Clear a poll rate override for a single address
bool DeviceIdentMgr::clearRateOverrideForAddress(BusElemAddrType address)
{
    uint32_t originalIntervalUs = 0;
    bool hadOverride = false;
    if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
    {
        auto it = _rateOverrideOriginalUs.find(address);
        if (it != _rateOverrideOriginalUs.end())
        {
            originalIntervalUs = it->second;
            _rateOverrideOriginalUs.erase(it);
            _rateOverridesUs.erase(address);
            hadOverride = true;
        }
        xSemaphoreGive(_offlineCtrlMutex);
    }
    if (!hadOverride)
        return false;

    DevicePollingInfo pollInfo;
    if (!_busStatusMgr.getDevicePollingInfo(address, pollInfo))
        return false;

    if (originalIntervalUs == 0)
        originalIntervalUs = pollInfo.pollIntervalUs;

    pollInfo.pollIntervalUs = originalIntervalUs;
    LOG_I(MODULE_PREFIX, "offline rate override clear addr %s restore intervalUs %u",
            BusI2CAddrAndSlot::toString(address).c_str(), pollInfo.pollIntervalUs);
    return _busStatusMgr.setDevicePollInterval(address, pollInfo.pollIntervalUs);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get NVS-backed offline count for an address
uint32_t DeviceIdentMgr::getOfflineNvsCount(BusElemAddrType address) const
{
    if (!_offlineNvsConfig.enabled)
        return 0;
    auto it = _offlineNvsStates.find(address);
    if (it == _offlineNvsStates.end())
        return 0;
    if (!it->second.store.isReady())
        return 0;
    return it->second.store.getCount();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Override per publish offline limit
void DeviceIdentMgr::setOfflineMaxPerPublishOverride(uint32_t maxPerPublish)
{
    _maxPerPublishOverride = maxPerPublish;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Set drain selection
void DeviceIdentMgr::setOfflineDrainSelection(const std::vector<BusElemAddrType>& addresses, const std::vector<std::string>& typeNames,
            bool drainOnlySelected)
{
    if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
    {
        _drainSelectedAddrs.clear();
        _drainSelectedTypes.clear();
        for (auto addr : addresses)
            _drainSelectedAddrs.insert(addr);
        for (const auto& typeName : typeNames)
        {
            if (!typeName.empty())
                _drainSelectedTypes.insert(typeName);
        }
        bool hasSelection = !_drainSelectedAddrs.empty() || !_drainSelectedTypes.empty();
        _drainOnlySelected = drainOnlySelected || hasSelection;
        // Selection alone should not auto-resume buffering/draining
        xSemaphoreGive(_offlineCtrlMutex);
    }
    applyOfflineControlToExisting();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Pause/resume buffering for addresses
void DeviceIdentMgr::setOfflineBufferPaused(const std::vector<BusElemAddrType>& addresses, bool paused)
{
    if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
    {
        if (addresses.empty())
        {
            _globalBufferPaused = paused;
            if (!paused)
                _bufferPausedAddrs.clear();
        }
        else
        {
            for (auto addr : addresses)
            {
                if (paused)
                    _bufferPausedAddrs.insert(addr);
                else
                    _bufferPausedAddrs.erase(addr);
            }
        }
        xSemaphoreGive(_offlineCtrlMutex);
    }

    if (addresses.empty())
        applyOfflineControlToExisting();
    else
    {
        for (auto addr : addresses)
        {
            uint16_t deviceTypeIdx = _busStatusMgr.getDeviceTypeIndexByAddr(addr);
            applyOfflineControlToAddress(addr, deviceTypeIdx);
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Estimate offline allocation bytes for addresses without applying changes
bool DeviceIdentMgr::estimateOfflineAllocations(const std::vector<BusElemAddrType>& addresses,
            std::map<BusElemAddrType, EstAllocInfo>& allocBytesOut) const
{
    allocBytesOut.clear();
    const uint32_t BUDGET_HEADROOM_BYTES = 1024 * 1024;
    const uint64_t MIN_TARGET_PER_DEVICE_BYTES = (uint64_t)5 * 1024 * 1024;
    bool explicitTargetsProvided = !addresses.empty();
    std::vector<BusElemAddrType> targetAddrs = addresses;
    if (targetAddrs.empty())
        _busStatusMgr.getBusElemAddresses(targetAddrs, false);
    if (targetAddrs.empty())
        return false;

    uint64_t freeMem = 0;
    uint64_t largestBlock = 0;
#ifdef ESP_PLATFORM
    freeMem = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (freeMem == 0)
        freeMem = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (largestBlock == 0)
        largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#endif
    if (freeMem == 0 && _busStatusMgr.getOfflineBytesInUse() == 0)
        return false;

    // Track current allocation and provisional sizing (mirrors start path which applies rate override then rebalances)
    std::map<BusElemAddrType, uint64_t> simCurrentAllocBytes;
    uint32_t currentBytes = 0;
    uint64_t provisionalConsumed = 0;
    for (auto addr : targetAddrs)
    {
        OfflineDataStats stats = _busStatusMgr.getOfflineStats(addr);
        DevicePollingInfo pollInfo;
        if (!_busStatusMgr.getDevicePollingInfo(addr, pollInfo))
            continue;
        uint32_t bytesPerEntry = pollInfo.pollResultSizeIncTimestamp + OfflineDataStore::META_STORAGE_BYTES;
        if (bytesPerEntry == 0)
            continue;
        uint64_t allocBytes = (uint64_t)stats.maxEntries * (stats.payloadSize + stats.metaSize);
        // Provisional allocation similar to rate override path (calc depth before rebalance)
        uint32_t provisionalDepth = computeDepthForAddress(addr, pollInfo);
        if (provisionalDepth == 0)
            provisionalDepth = _offlinePolicy.minSamples > 0 ? _offlinePolicy.minSamples : 1;
        uint64_t provisionalAlloc = (uint64_t)provisionalDepth * bytesPerEntry;
        if (provisionalAlloc > allocBytes)
        {
            provisionalConsumed += provisionalAlloc - allocBytes;
            allocBytes = provisionalAlloc;
        }
        simCurrentAllocBytes[addr] = allocBytes;
        currentBytes += (uint32_t)std::min<uint64_t>(allocBytes, UINT32_MAX);
    }

    // Approximate free memory after provisional allocation (as happens during rate override)
    uint64_t freeMemSim = freeMem;
    uint64_t largestBlockSim = largestBlock;
    if (provisionalConsumed > 0)
    {
        freeMemSim = freeMemSim > provisionalConsumed ? (freeMemSim - provisionalConsumed) : 0;
        largestBlockSim = largestBlockSim > provisionalConsumed ? (largestBlockSim - provisionalConsumed) : freeMemSim;
    }
    if (largestBlockSim > freeMemSim)
        largestBlockSim = freeMemSim;

    uint64_t budgetBytes = currentBytes;
    if (_offlinePolicy.memUsePermille > 0)
        budgetBytes += (freeMemSim * _offlinePolicy.memUsePermille) / 1000;
    else
        budgetBytes += freeMemSim;
    if ((_offlinePolicy.globalMaxBytes > 0) && (budgetBytes > _offlinePolicy.globalMaxBytes))
        budgetBytes = _offlinePolicy.globalMaxBytes;
    budgetBytes = (budgetBytes > BUDGET_HEADROOM_BYTES) ? (budgetBytes - BUDGET_HEADROOM_BYTES) : 0;
    if (explicitTargetsProvided)
    {
        uint64_t minTotalBudget = MIN_TARGET_PER_DEVICE_BYTES * targetAddrs.size();
        if ((budgetBytes < minTotalBudget) && ((budgetBytes + BUDGET_HEADROOM_BYTES) >= minTotalBudget))
            budgetBytes = minTotalBudget;
    }
    uint64_t perDeviceBudget = targetAddrs.empty() ? 0 : budgetBytes / targetAddrs.size();
    if (explicitTargetsProvided)
    {
        if ((budgetBytes >= MIN_TARGET_PER_DEVICE_BYTES * targetAddrs.size()) && (perDeviceBudget < MIN_TARGET_PER_DEVICE_BYTES))
            perDeviceBudget = MIN_TARGET_PER_DEVICE_BYTES;
    }
    if ((_offlinePolicy.perDeviceMaxBytes > 0) && (perDeviceBudget > _offlinePolicy.perDeviceMaxBytes))
        perDeviceBudget = _offlinePolicy.perDeviceMaxBytes;

    uint64_t freeMemRemaining = freeMemSim;
    uint64_t largestBlockRemaining = largestBlockSim;
    if (largestBlockRemaining > freeMemRemaining)
        largestBlockRemaining = freeMemRemaining;

    for (auto addr : targetAddrs)
    {
        DevicePollingInfo pollInfo;
        if (!_busStatusMgr.getDevicePollingInfo(addr, pollInfo))
            continue;
        uint32_t bytesPerEntry = pollInfo.pollResultSizeIncTimestamp + OfflineDataStore::META_STORAGE_BYTES;
        if (bytesPerEntry == 0)
            continue;
        OfflineDataStats stats = _busStatusMgr.getOfflineStats(addr);
        uint64_t currentAllocBytes = simCurrentAllocBytes.count(addr) ?
                    simCurrentAllocBytes[addr] :
                    (uint64_t)stats.maxEntries * (stats.payloadSize + stats.metaSize);
        uint64_t targetBudget = perDeviceBudget;
        if (currentAllocBytes > targetBudget)
            targetBudget = currentAllocBytes;
#ifdef ESP_PLATFORM
        uint32_t depth = bytesPerEntry ? (uint32_t)(targetBudget / bytesPerEntry) : 0;
        if (depth == 0)
            depth = _offlinePolicy.minSamples > 0 ? _offlinePolicy.minSamples : 1;
        uint32_t maxDepthFromBlocks = depth;
        if (largestBlockRemaining > 0)
        {
            uint32_t maxDepthFromRing = pollInfo.pollResultSizeIncTimestamp ?
                    (uint32_t)(largestBlockRemaining / pollInfo.pollResultSizeIncTimestamp) : depth;
            uint32_t maxDepthFromMeta = OfflineDataStore::META_STORAGE_BYTES ?
                    (uint32_t)(largestBlockRemaining / OfflineDataStore::META_STORAGE_BYTES) : depth;
            maxDepthFromBlocks = std::min(maxDepthFromRing, maxDepthFromMeta);
            uint64_t freeOrLargest = (largestBlockRemaining > freeMemRemaining) ? largestBlockRemaining : freeMemRemaining;
            uint32_t maxDepthFromFree = bytesPerEntry ? (uint32_t)(freeOrLargest / bytesPerEntry) : depth;
            if (maxDepthFromFree < maxDepthFromBlocks)
                maxDepthFromBlocks = maxDepthFromFree;
        }
        if (maxDepthFromBlocks > 0 && depth > maxDepthFromBlocks)
            depth = maxDepthFromBlocks;
#else
        uint32_t depth = bytesPerEntry ? (uint32_t)(targetBudget / bytesPerEntry) : 0;
        if (depth == 0)
            depth = _offlinePolicy.minSamples > 0 ? _offlinePolicy.minSamples : 1;
#endif
        // Never report an estimate below the current allocation (already reserved)
        uint32_t currentDepth = bytesPerEntry ? (uint32_t)(currentAllocBytes / bytesPerEntry) : 0;
        if ((currentDepth > 0) && (currentDepth > depth))
            depth = currentDepth;
        uint64_t allocBytes = (uint64_t)depth * bytesPerEntry;
        EstAllocInfo info;
        info.allocBytes = allocBytes > UINT32_MAX ? UINT32_MAX : (uint32_t)allocBytes;
        info.bytesPerEntry = bytesPerEntry;
        info.payloadSize = pollInfo.pollResultSizeIncTimestamp;
        info.metaSize = OfflineDataStore::META_STORAGE_BYTES;
        allocBytesOut[addr] = info;

        // Simulate memory consumption so later targets see reduced free space
        if (allocBytes >= currentAllocBytes)
        {
            uint64_t delta = allocBytes - currentAllocBytes;
            freeMemRemaining = freeMemRemaining > delta ? (freeMemRemaining - delta) : 0;
            uint64_t largestConsume = std::max((uint64_t)depth * pollInfo.pollResultSizeIncTimestamp,
                        (uint64_t)depth * OfflineDataStore::META_STORAGE_BYTES);
            largestBlockRemaining = largestBlockRemaining > largestConsume ? (largestBlockRemaining - largestConsume) : freeMemRemaining;
        }
        else
        {
            uint64_t delta = currentAllocBytes - allocBytes;
            freeMemRemaining += delta;
            largestBlockRemaining += delta;
        }
        if (largestBlockRemaining > freeMemRemaining)
            largestBlockRemaining = freeMemRemaining;
    }
    return !allocBytesOut.empty();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Pause/resume draining for addresses
void DeviceIdentMgr::setOfflineDrainPaused(const std::vector<BusElemAddrType>& addresses, bool paused)
{
    if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
    {
        if (addresses.empty())
        {
            _globalDrainPaused = paused;
            if (!paused)
                _drainPausedAddrs.clear();
        }
        else
        {
            for (auto addr : addresses)
            {
                if (paused)
                    _drainPausedAddrs.insert(addr);
                else
                    _drainPausedAddrs.erase(addr);
            }
        }
        xSemaphoreGive(_offlineCtrlMutex);
    }

    if (addresses.empty())
        applyOfflineControlToExisting();
    else
    {
        for (auto addr : addresses)
        {
            uint16_t deviceTypeIdx = _busStatusMgr.getDeviceTypeIndexByAddr(addr);
            applyOfflineControlToAddress(addr, deviceTypeIdx);
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Pause/resume draining based on link availability
void DeviceIdentMgr::setOfflineDrainLinkPaused(bool paused)
{
    bool changed = false;
    if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
    {
        if (_linkDrainPaused != paused)
        {
            _linkDrainPaused = paused;
            changed = true;
        }
        xSemaphoreGive(_offlineCtrlMutex);
    }
    else if (_linkDrainPaused != paused)
    {
        _linkDrainPaused = paused;
        changed = true;
    }

    if (changed)
        applyOfflineControlToExisting();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Persist auto-resume recording state for reboot
void DeviceIdentMgr::setOfflineAutoResume(bool enabled, const std::vector<BusElemAddrType>& addresses, uint32_t rateOverrideMs)
{
    if (!_offlineNvsConfig.enabled)
        return;

    OfflineResumeState nextState;
    nextState.active = enabled;
    nextState.rateOverrideMs = rateOverrideMs;
    nextState.targetAddrs.clear();
    if (enabled)
    {
        for (auto addr : addresses)
            nextState.targetAddrs.insert(addr);
    }

    if (nextState.rateOverrideMs < 10)
        nextState.rateOverrideMs = 0;
    if (nextState.rateOverrideMs > 60000)
        nextState.rateOverrideMs = 60000;

    if (nextState.targetAddrs.empty())
    {
        nextState.active = false;
        nextState.rateOverrideMs = 0;
    }

    if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
    {
        _offlineResume = nextState;
        _offlineResumePending.clear();
        xSemaphoreGive(_offlineCtrlMutex);
    }
    else
    {
        _offlineResume = nextState;
        _offlineResumePending.clear();
    }

    saveOfflineResumeState();
    LOG_I(MODULE_PREFIX, "offline auto-resume %s targets %u rateMs %u",
            _offlineResume.active ? "enabled" : "disabled",
            (unsigned)_offlineResume.targetAddrs.size(),
            (unsigned)_offlineResume.rateOverrideMs);
}

/// @brief Reset offline buffers for addresses
void DeviceIdentMgr::resetOfflineBuffers(const std::vector<BusElemAddrType>& addresses)
{
    std::vector<BusElemAddrType> targetAddrs = addresses;
    if (targetAddrs.empty())
        _busStatusMgr.getBusElemAddresses(targetAddrs, false);

    for (auto addr : targetAddrs)
    {
        _busStatusMgr.resetOfflineBuffer(addr);
        DevicePollingInfo pollInfo;
        if (_busStatusMgr.getDevicePollingInfo(addr, pollInfo))
        {
            // Unconfigure buffer to release allocation; reconfigured on start
            _busStatusMgr.reconfigureOfflineBuffer(addr, 0,
                    pollInfo.pollResultSizeIncTimestamp,
                    DevicePollingInfo::POLL_RESULT_TIMESTAMP_SIZE,
                    DevicePollingInfo::POLL_RESULT_RESOLUTION_US);
            LOG_I(MODULE_PREFIX, "resetOfflineBuffers addr %s unconfigured",
                    BusI2CAddrAndSlot::toString(addr).c_str());
        }
        // Ensure buffering/draining stays paused after reset
        _busStatusMgr.setOfflineBufferPaused(addr, true);
        _busStatusMgr.setOfflineDrainPaused(addr, true);
        if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
        {
            _bufferPausedAddrs.insert(addr);
            _drainPausedAddrs.insert(addr);
            xSemaphoreGive(_offlineCtrlMutex);
        }
    }

    clearOfflineNvsState(targetAddrs);

    applyOfflineControlToExisting();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Snapshot control state for diagnostics
void DeviceIdentMgr::getOfflineControlSnapshot(std::set<BusElemAddrType>& bufferPaused, std::set<BusElemAddrType>& drainPaused,
            std::set<BusElemAddrType>& drainSelectedAddrs, std::set<std::string>& drainSelectedTypes,
            bool& drainOnlySelected, uint32_t& maxPerPublishOverride,
            bool& globalBufferPaused, bool& globalDrainPaused,
            std::map<BusElemAddrType, uint32_t>& rateOverridesUs) const
{
    bufferPaused.clear();
    drainPaused.clear();
    drainSelectedAddrs.clear();
    drainSelectedTypes.clear();
    drainOnlySelected = _drainOnlySelected;
    maxPerPublishOverride = _maxPerPublishOverride;
    globalBufferPaused = _globalBufferPaused;
    globalDrainPaused = _globalDrainPaused || _linkDrainPaused;
    rateOverridesUs.clear();

    if (_offlineCtrlMutex && (xSemaphoreTake(_offlineCtrlMutex, pdMS_TO_TICKS(5)) == pdTRUE))
    {
        bufferPaused = _bufferPausedAddrs;
        drainPaused = _drainPausedAddrs;
        drainSelectedAddrs = _drainSelectedAddrs;
        drainSelectedTypes = _drainSelectedTypes;
        drainOnlySelected = _drainOnlySelected;
        maxPerPublishOverride = _maxPerPublishOverride;
        globalBufferPaused = _globalBufferPaused;
        globalDrainPaused = _globalDrainPaused || _linkDrainPaused;
        rateOverridesUs = _rateOverridesUs;
        xSemaphoreGive(_offlineCtrlMutex);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get list of device addresses attached to the bus
/// @param pAddrList pointer to array to receive addresses
/// @param onlyAddressesWithIdentPollResponses true to only return addresses with ident poll responses    
void DeviceIdentMgr::getDeviceAddresses(std::vector<BusElemAddrType>& addresses, bool onlyAddressesWithIdentPollResponses) const
{
    // Get list of all bus element addresses
    _busStatusMgr.getBusElemAddresses(addresses, onlyAddressesWithIdentPollResponses);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get device type name for an address
bool DeviceIdentMgr::getDeviceTypeName(BusElemAddrType address, std::string& typeName) const
{
    typeName.clear();
    uint16_t deviceTypeIdx = _busStatusMgr.getDeviceTypeIndexByAddr(address);
    DeviceTypeRecord devTypeRec;
    if (!deviceTypeRecords.getDeviceInfo(deviceTypeIdx, devTypeRec) || !devTypeRec.deviceType)
        return false;
    typeName = devTypeRec.deviceType;
    return true;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Identify device
/// @param address address of device
/// @param deviceStatus (out) device status
/// @note This is called from within the scanning code so the device should already be selected if it is on a bus extender, etc.
void DeviceIdentMgr::identifyDevice(BusElemAddrType address, DeviceStatus& deviceStatus)
{
    // Clear device status
    deviceStatus.clear();

    // Check if enabled
    if (!_isEnabled)
    {
#ifdef DEBUG_DEVICE_IDENT_MGR
        LOG_I(MODULE_PREFIX, "Device identification disabled");
#endif
        return;
    }

    // Get the raw I2C address (excluding slot number)
    uint32_t i2cAddr = BusI2CAddrAndSlot::getI2CAddr(address);

    // Check if this address is in the range of any known device
    std::vector<uint16_t> deviceTypesForAddr = deviceTypeRecords.getDeviceTypeIdxsForAddr(i2cAddr);
    for (const auto& deviceTypeIdx : deviceTypesForAddr)
    {
        // Get JSON definition for device
        DeviceTypeRecord devTypeRec;
        if (!deviceTypeRecords.getDeviceInfo(deviceTypeIdx, devTypeRec))
            continue;

#ifdef DEBUG_DEVICE_IDENT_MGR
        LOG_I(MODULE_PREFIX, "identifyDevice potential deviceType %s address %s", 
                    devTypeRec.deviceType ? devTypeRec.deviceType : "NO NAME", BusI2CAddrAndSlot::toString(address).c_str());
#endif

        // Check if the detection value(s) match responses from the device
        // Generate a bus request to read the detection value
        if (checkDeviceTypeMatch(address, &devTypeRec))
        {
#ifdef DEBUG_DEVICE_IDENT_MGR_DETAIL
            LOG_I(MODULE_PREFIX, "identifyDevice FOUND %s", devTypeRec.devInfoJson ? devTypeRec.devInfoJson : "NO INFO");
#endif
#ifdef INFO_NEW_DEVICE_IDENTIFIED
            LOG_I(MODULE_PREFIX, "identifyDevice new device %s at address %s", 
                    devTypeRec.deviceType ? devTypeRec.deviceType : "NO NAME", 
                    BusI2CAddrAndSlot::toString(address).c_str());
#endif

            // Initialise the device if required
            processDeviceInit(address, &devTypeRec);

            // Set device type index
            deviceStatus.deviceTypeIndex = deviceTypeIdx;

            // Get polling info
            deviceTypeRecords.getPollInfo(address, &devTypeRec, deviceStatus.deviceIdentPolling);

            // Set polling results size
            deviceStatus.dataAggregator.init(deviceStatus.deviceIdentPolling.numPollResultsToStore, 
                    deviceStatus.deviceIdentPolling.pollResultSizeIncTimestamp);

            // Defer offline buffer allocation until an explicit offlinebuf start command
            deviceStatus.setOfflineBufferPaused(true);
            if (_offlineResume.active)
                queueOfflineResume(address);
            applyOfflineControlsToDevice(address, deviceStatus);
            if (_offlineNvsConfig.enabled && _offlineNvsConfig.importOnBoot)
            {
                LOG_I(MODULE_PREFIX, "offline NVS import deferred addr %s (offlinebuf start required)",
                        BusI2CAddrAndSlot::toString(address).c_str());
            }

#ifdef DEBUG_HANDLE_BUS_DEVICE_INFO
            LOG_I(MODULE_PREFIX, "setBusElemDevInfo address %s numPollResToStore %d pollResSizeIncTimestamp %d", 
                    BusI2CAddrAndSlot::toString(address).c_str(),
                    deviceStatus.deviceIdentPolling.numPollResultsToStore,
                    deviceStatus.deviceIdentPolling.pollResultSizeIncTimestamp);
#endif
            // Break out of the loop
            break;
        }
        else
        {
#ifdef DEBUG_DEVICE_IDENT_MGR_DETAIL
            LOG_I(MODULE_PREFIX, "identifyDevice CHECK FAILED %s", devTypeRec.devInfoJson ? devTypeRec.devInfoJson : "NO INFO");
#endif
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Access device and check response
///////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool DeviceIdentMgr::checkDeviceTypeMatch(BusElemAddrType address, const DeviceTypeRecord* pDevTypeRec)
{
    // Get the detection records
    std::vector<DeviceTypeRecords::DeviceDetectionRec> detectionRecs;
    deviceTypeRecords.getDetectionRecs(pDevTypeRec, detectionRecs);

    // Check if all values match
    bool detectionValuesMatch = true;
    for (const auto& detectionRec : detectionRecs)
    {

        // Check there is a read data check
        uint32_t readDataCheckBytes = 0;
        if (detectionRec.checkValues.size() == 0)
            continue;
        readDataCheckBytes = detectionRec.checkValues[0].second.size();

        // Create a bus request to read the detection value
        // Create the poll request
        BusRequestInfo reqRec(BUS_REQ_TYPE_FAST_SCAN, 
                address,
                0, 
                detectionRec.writeData.size(), 
                detectionRec.writeData.data(),
                readDataCheckBytes,
                detectionRec.pauseAfterSendMs, 
                nullptr, 
                this);
        std::vector<uint8_t> readData;
        RaftRetCode rslt = _busReqSyncFn != nullptr ? _busReqSyncFn(&reqRec, &readData) : RAFT_BUS_NOT_INIT;

#ifdef DEBUG_DEVICE_IDENT_MGR
        String writeStr;
        Raft::getHexStrFromBytes(detectionRec.writeData.data(), detectionRec.writeData.size(), writeStr);
        String readDataStr;
        Raft::getHexStrFromBytes(readData.data(), readData.size(), readDataStr);
        LOG_I(MODULE_PREFIX, "checkDeviceTypeMatch %s addr %s writeData %s rslt %d readData %s readSize %d pauseAfterMs %d", 
                    rslt == RAFT_OK ? "OK" : "BUS ACCESS FAILED",
                    BusI2CAddrAndSlot::toString(address).c_str(), 
                    writeStr.c_str(), rslt, 
                    readDataStr.c_str(), readData.size(), 
                    detectionRec.pauseAfterSendMs);
#endif

        // Check ok result
        if (rslt != RAFT_OK)
            return false;

        // Iterate through check values to see if one of them matches
        bool checkValueMatch = false;
        for (const auto& checkValue : detectionRec.checkValues)
        {

#ifdef DEBUG_DEVICE_IDENT_MGR
            String readMaskStr;
            Raft::getHexStrFromBytes(checkValue.first.data(), checkValue.first.size(), readMaskStr);
            String readCheckStr;
            Raft::getHexStrFromBytes(checkValue.second.data(), checkValue.second.size(), readCheckStr);
            LOG_I(MODULE_PREFIX, "checkDeviceTypeMatch readDataMask %s readDataCheck %s VS readData %s",
                        readMaskStr.c_str(),
                        readCheckStr.c_str(),
                        readDataStr.c_str());
#endif

            // Check the read data
            bool sizeMatch = readData.size() == checkValue.second.size();
            if (sizeMatch)
            {
                bool checkByteMatch = true;
                for (int i = 0; i < readData.size(); i++)
                {
                    if ((readData[i] & checkValue.first[i]) != checkValue.second[i])
                    {
                        checkByteMatch = false;
                        break;
                    }
                }
                if (checkByteMatch)
                {
                    checkValueMatch = true;
                    break;
                }
            }

#ifdef DEBUG_DEVICE_IDENT_MGR
            LOG_I(MODULE_PREFIX, "checkDeviceTypeMatch readData %s sizeMatch %d checkValueMatch %d", 
                        readDataStr.c_str(), sizeMatch, checkValueMatch);
#endif
        }

#ifdef DEBUG_DEVICE_IDENT_MGR
        LOG_I(MODULE_PREFIX, "checkDeviceTypeMatch address %s %s", 
                    BusI2CAddrAndSlot::toString(address).c_str(),
                    checkValueMatch ? "MATCH" : "NO MATCH");
#endif

        // Check if all values match
        if (!checkValueMatch)
            detectionValuesMatch = false;

        if (detectionRec.pauseAfterSendMs > 0)
            delay(detectionRec.pauseAfterSendMs);
    }

    // Access the device and check the response
    return detectionValuesMatch;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process initialisation of a device
///////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool DeviceIdentMgr::processDeviceInit(BusElemAddrType address, const DeviceTypeRecord* pDevTypeRec)
{
    // Get initialisation bus requests
    std::vector<BusRequestInfo> initBusRequests;
    deviceTypeRecords.getInitBusRequests(address, pDevTypeRec, initBusRequests);

#ifdef DEBUG_DEVICE_IDENT_MGR
    LOG_I(MODULE_PREFIX, "processDeviceInit address %s numInitBusRequests %d", 
                BusI2CAddrAndSlot::toString(address).c_str(), initBusRequests.size());
#endif

    // Initialise the device
    for (auto& initBusRequest : initBusRequests)
    {
        std::vector<uint8_t> readData;
        BusRequestInfo reqRec(initBusRequest);
        if (_busReqSyncFn != nullptr)
            _busReqSyncFn(&reqRec, &readData);

        // Check for bar-access time after each request
        if (initBusRequest.getBarAccessForMsAfterSend() > 0)
            delay(initBusRequest.getBarAccessForMsAfterSend());
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Format device poll responses to JSON
///////////////////////////////////////////////////////////////////////////////////////////////////////////////

String DeviceIdentMgr::deviceStatusToJson(BusElemAddrType address, bool isOnline, uint16_t deviceTypeIndex, 
                const std::vector<uint8_t>& devicePollResponseData, uint32_t responseSize,
                bool isBacklog, uint32_t remainingCount, const OfflineDataMeta* pFirstMeta,
                const OfflineDataStats& stats) const
{
    // Get device type info
    DeviceTypeRecord devTypeRec;
    if (!deviceTypeRecords.getDeviceInfo(deviceTypeIndex, devTypeRec))
        return "";

    // Get the poll response JSON
    String jsonOut = deviceTypeRecords.deviceStatusToJson(address, isOnline, &devTypeRec, devicePollResponseData,
                isBacklog, remainingCount, pFirstMeta, &stats);
    if (isBacklog)
    {
        String snippet = jsonOut;
        if (snippet.length() > 120)
            snippet = snippet.substring(0, 120);
        LOG_I(MODULE_PREFIX, "offline backlog json addr 0x%x type %s remain %u json %s...",
                (unsigned)address,
                devTypeRec.deviceType ? devTypeRec.deviceType : "unknown",
                (unsigned)remainingCount,
                snippet.c_str());
    }
    return jsonOut;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get JSON for device type info
/// @param address Address of element
/// @return JSON string
String DeviceIdentMgr::getDevTypeInfoJsonByAddr(BusElemAddrType address, bool includePlugAndPlayInfo) const
{
    // Get device type index
    uint16_t deviceTypeIdx = _busStatusMgr.getDeviceTypeIndexByAddr(address);
    if (deviceTypeIdx == DeviceStatus::DEVICE_TYPE_INDEX_INVALID)
        return "{}";

    // Get device type info
    return deviceTypeRecords.getDevTypeInfoJsonByTypeIdx(deviceTypeIdx, includePlugAndPlayInfo);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get JSON for device type info
/// @param deviceType Device type
/// @return JSON string
String DeviceIdentMgr::getDevTypeInfoJsonByTypeName(const String& deviceType, bool includePlugAndPlayInfo) const
{
    // Get device type info
    return deviceTypeRecords.getDevTypeInfoJsonByTypeName(deviceType, includePlugAndPlayInfo);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get device type info JSON by device type index
/// @param deviceTypeIdx device type index
/// @param includePlugAndPlayInfo include plug and play info
/// @return JSON string
String DeviceIdentMgr::getDevTypeInfoJsonByTypeIdx(uint16_t deviceTypeIdx, bool includePlugAndPlayInfo) const
{
    // Get device type info
    return deviceTypeRecords.getDevTypeInfoJsonByTypeIdx(deviceTypeIdx, includePlugAndPlayInfo);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Peek offline data without consuming
String DeviceIdentMgr::peekOfflineDataJson(const std::vector<BusElemAddrType>& addresses,
            uint32_t startIdx, uint32_t maxResponsesToReturn, uint32_t maxBytes,
            uint32_t& totalRemaining)
{
    String jsonStr;
    totalRemaining = 0;

    ensureOfflineNvsForPeek(addresses);

    std::vector<BusElemAddrType> targetAddrs = addresses;
    if (targetAddrs.empty())
        _busStatusMgr.getBusElemAddresses(targetAddrs, false);

    uint32_t maxBytesPerDevice = maxBytes;
    if (maxBytesPerDevice == 0)
        maxBytesPerDevice = _offlinePolicy.perDeviceMaxBytes ? _offlinePolicy.perDeviceMaxBytes : 2048;

    for (auto address : targetAddrs)
    {
        bool isOnline = false;
        uint16_t deviceTypeIndex = 0;
        std::vector<uint8_t> devicePollResponseData;
        uint32_t responseSize = 0;
        std::vector<OfflineDataMeta> metas;
        OfflineDataStats stats;

        uint32_t numResponses = _busStatusMgr.peekBusElemOfflineResponses(address, isOnline, deviceTypeIndex,
                    devicePollResponseData, responseSize, startIdx, maxResponsesToReturn, maxBytesPerDevice, metas, stats);

        uint32_t remaining = (stats.depth > (startIdx + numResponses)) ? (stats.depth - (startIdx + numResponses)) : 0;
        if (stats.depth > startIdx)
            totalRemaining += stats.depth - startIdx;

        if (stats.depth > 0 && (numResponses == 0 || numResponses < stats.depth))
        {
            LOG_I(MODULE_PREFIX,
                  "offline peek addr %s depth %u start %u returned %u remaining %u maxResponses %u maxBytes %u respSize %u",
                  BusI2CAddrAndSlot::toString(address).c_str(),
                  stats.depth, startIdx, numResponses, remaining,
                  maxResponsesToReturn, maxBytesPerDevice, responseSize);
        }

        if ((numResponses == 0) && (remaining == 0))
            continue;

        String jsonData = deviceStatusToJson(address,
                        isOnline, deviceTypeIndex, devicePollResponseData, responseSize,
                        true, remaining, metas.size() > 0 ? &metas.front() : nullptr, stats);
        if (jsonData.length() > 0)
        {
            jsonStr += (jsonStr.length() == 0 ? "{" : ",") + jsonData;
        }
    }

    return jsonStr.length() == 0 ? "{}" : jsonStr + "}";
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get queued device data in JSON format
/// @return JSON doc
String DeviceIdentMgr::getQueuedDeviceDataJson(uint32_t maxResponsesToReturn, uint32_t* pRemaining) const
{
    // Return string
    String jsonStr;
    uint32_t remainingTotal = 0;
    uint32_t perDeviceLimit = getPerDevicePublishLimit(maxResponsesToReturn);

    // Get list of all bus element addresses
    std::vector<BusElemAddrType> addresses;
    _busStatusMgr.getBusElemAddresses(addresses, false);
    for (auto address : addresses)
    {
        // Get bus status for each address
        bool isOnline = false;
        uint16_t deviceTypeIndex = _busStatusMgr.getDeviceTypeIndexByAddr(address);
        DeviceTypeRecord devTypeRec;
        const char* devTypeName = nullptr;
        if (deviceTypeRecords.getDeviceInfo(deviceTypeIndex, devTypeRec))
            devTypeName = devTypeRec.deviceType;
        std::vector<uint8_t> devicePollResponseData;
        uint32_t responseSize = 0;
        std::vector<OfflineDataMeta> metas;
        OfflineDataStats stats;
        bool isBacklog = false;
        uint32_t numResponses = 0;

        // Drain offline backlog if allowed
        bool drainAllowed = isOfflineDrainAllowed(address, deviceTypeIndex);
        if (drainAllowed)
        {
            numResponses = _busStatusMgr.getBusElemOfflineResponses(address, isOnline, deviceTypeIndex, 
                        devicePollResponseData, responseSize, perDeviceLimit, metas, stats);
            isBacklog = numResponses > 0;
            remainingTotal += stats.depth;
            if (numResponses == 0)
            {
                numResponses = _busStatusMgr.getBusElemPollResponses(address, isOnline, deviceTypeIndex, 
                            devicePollResponseData, responseSize, perDeviceLimit);
            }
        }
        else
        {
            // Drain suppressed - still report remaining depth for backlog hints
            stats = _busStatusMgr.getOfflineStats(address);
            remainingTotal += stats.depth;
            numResponses = _busStatusMgr.getBusElemPollResponses(address, isOnline, deviceTypeIndex, 
                        devicePollResponseData, responseSize, perDeviceLimit);
        }

        if ((stats.depth > 0) || (numResponses > 0) || !drainAllowed)
        {
            // LOG_I(MODULE_PREFIX, "offlinebuf publish addr 0x%x type %s drainAllowed %d backlogDepth %u responses %u remainTotal %u",
            //         (unsigned)address,
            //         devTypeName ? devTypeName : "unknown",
            //         drainAllowed,
            //         (unsigned)stats.depth,
            //         (unsigned)numResponses,
            //         (unsigned)remainingTotal);
        }

        // Use device identity manager to convert to JSON
        String jsonData = deviceStatusToJson(address, 
                        isOnline, deviceTypeIndex, devicePollResponseData, responseSize,
                        isBacklog, stats.depth, metas.size() > 0 ? &metas.front() : nullptr, stats);
        if (jsonData.length() > 0)
        {
            jsonStr += (jsonStr.length() == 0 ? "{" : ",") + jsonData;
        }
    }
    setOfflineStatsRemaining(remainingTotal, pRemaining);
    return jsonStr.length() == 0 ? "{}" : jsonStr + "}";
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get queued device data in binary format
/// @param connMode connection mode (inc bus number)
/// @return Binary data vector
std::vector<uint8_t> DeviceIdentMgr::getQueuedDeviceDataBinary(uint32_t connMode, uint32_t maxResponsesToReturn,
            uint32_t* pRemaining) const
{
    // Return buffer
    std::vector<uint8_t> binData;
    uint32_t remainingTotal = 0;
    uint32_t perDeviceLimit = getPerDevicePublishLimit(maxResponsesToReturn);

    // Get list of all bus element addresses
    std::vector<BusElemAddrType> addresses;
    _busStatusMgr.getBusElemAddresses(addresses, false);
    for (auto address : addresses)
    {
        // Get bus status for each address
        bool isOnline = false;
        uint16_t deviceTypeIndex = _busStatusMgr.getDeviceTypeIndexByAddr(address);
        std::vector<uint8_t> devicePollResponseData;
        uint32_t responseSize = 0;
        std::vector<OfflineDataMeta> metas;
        OfflineDataStats stats;
        uint32_t numResponses = 0;

        if (isOfflineDrainAllowed(address, deviceTypeIndex))
        {
            numResponses = _busStatusMgr.getBusElemOfflineResponses(address, isOnline, deviceTypeIndex, 
                        devicePollResponseData, responseSize, perDeviceLimit, metas, stats);
            remainingTotal += stats.depth;
            if (numResponses == 0)
            {
                numResponses = _busStatusMgr.getBusElemPollResponses(address, isOnline, deviceTypeIndex, devicePollResponseData, responseSize, perDeviceLimit);
            }
        }
        else
        {
            stats = _busStatusMgr.getOfflineStats(address);
            remainingTotal += stats.depth;
            numResponses = _busStatusMgr.getBusElemPollResponses(address, isOnline, deviceTypeIndex, devicePollResponseData, responseSize, perDeviceLimit);
        }

        // Get poll response JSON
        if (devicePollResponseData.size() > 0)
        {
            // Generate binary device message
            RaftDevice::genBinaryDataMsg(binData, connMode, address, deviceTypeIndex, isOnline, devicePollResponseData);
        }
    }

    setOfflineStatsRemaining(remainingTotal, pRemaining);

    // Return binary data
    return binData;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get decoded poll responses
/// @param address address of device to get data from
/// @param pStructOut pointer to structure (or array of structures) to receive decoded data
/// @param structOutSize size of structure (in bytes) to receive decoded data
/// @param maxRecCount maximum number of records to decode
/// @param decodeState decode state for this device
/// @return number of records decoded
/// @note the pStructOut should generally point to structures of the correct type for the device data and the
///       decodeState should be maintained between calls for the same device
uint32_t DeviceIdentMgr::getDecodedPollResponses(BusElemAddrType address, 
                void* pStructOut, uint32_t structOutSize, 
                uint16_t maxRecCount, RaftBusDeviceDecodeState& decodeState) const
{
    // Get poll result for each address
    bool isOnline = false;
    uint16_t deviceTypeIndex = 0;
    std::vector<uint8_t> devicePollResponseData;
    uint32_t responseSize = 0;
    _busStatusMgr.getBusElemPollResponses(address, isOnline, deviceTypeIndex, devicePollResponseData, responseSize, 0);

#ifdef DEBUG_GET_DECODED_POLL_RESPONSES
    LOG_I(MODULE_PREFIX, "getDecodedPollResponses address %s isOnline %d deviceTypeIndex %d responseSize %d",
                BusI2CAddrAndSlot::toString(address).c_str(),
                isOnline, deviceTypeIndex, responseSize);
#endif

    // Decode the poll response
    return decodePollResponses(deviceTypeIndex, devicePollResponseData.data(), responseSize, 
                pStructOut, structOutSize, 
                maxRecCount, decodeState);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get debug JSON
/// @return JSON string
String DeviceIdentMgr::getDebugJSON(bool includeBraces) const
{
    return _busStatusMgr.getDebugJSON(includeBraces);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Decode one or more poll responses for a device
/// @param deviceTypeIndex index of device type
/// @param pPollBuf buffer containing poll responses
/// @param pollBufLen length of poll response buffer
/// @param pStructOut pointer to structure (or array of structures) to receive decoded data
/// @param structOutSize size of structure (in bytes) to receive decoded data (includes timestamp)
/// @param maxRecCount maximum number of records to decode
/// @return number of records decoded
uint32_t DeviceIdentMgr::decodePollResponses(uint16_t deviceTypeIndex, 
            const uint8_t* pPollBuf, uint32_t pollBufLen, 
            void* pStructOut, uint32_t structOutSize, 
            uint16_t maxRecCount, RaftBusDeviceDecodeState& decodeState) const
{
    // Get device type info
    DeviceTypeRecord devTypeRec;
    if (!deviceTypeRecords.getDeviceInfo(deviceTypeIndex, devTypeRec))
        return 0;

    // Check the decode method is present
    if (!devTypeRec.pollResultDecodeFn)
        return 0;

    // Decode the poll response
    return devTypeRec.pollResultDecodeFn(pPollBuf, pollBufLen, pStructOut, structOutSize, maxRecCount, decodeState);
}
