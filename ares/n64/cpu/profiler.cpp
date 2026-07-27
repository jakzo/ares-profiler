namespace {

auto readBigEndian16(const std::vector<u8>& data, size_t offset) -> u16 {
  if(offset + 2 > data.size()) return 0;
  return u16(data[offset]) << 8 | u16(data[offset + 1]);
}

auto readBigEndian32(const std::vector<u8>& data, size_t offset) -> u32 {
  if(offset + 4 > data.size()) return 0;
  return u32(data[offset]) << 24 | u32(data[offset + 1]) << 16
       | u32(data[offset + 2]) << 8 | u32(data[offset + 3]);
}

auto readBigEndian64(const std::vector<u8>& data, size_t offset) -> u64 {
  if(offset + 8 > data.size()) return 0;
  return u64(readBigEndian32(data, offset)) << 32
       | u64(readBigEndian32(data, offset + 4));
}

auto symbolName(const std::vector<u8>& data, size_t offset, size_t limit) -> std::string {
  std::string result;
  while(offset < limit && offset < data.size() && data[offset]) {
    result += char(data[offset++]);
  }
  return result;
}

constexpr const char* DroppedFrameMetrics[] = {
  "dropped_frames_0",
  "dropped_frames_1",
  "dropped_frames_2",
  "dropped_frames_3",
  "dropped_frames_4",
  "dropped_frames_5_6",
  "dropped_frames_7_8",
  "dropped_frames_9_10",
  "dropped_frames_11_plus",
};

auto droppedFrameBucket(u64 argument) -> size_t {
  auto deltaFrames = s32(u32(argument));
  auto droppedFrames = deltaFrames > 1 ? u32(deltaFrames - 1) : 0;
  if(droppedFrames <= 4) return size_t(droppedFrames);
  if(droppedFrames <= 6) return 5;
  if(droppedFrames <= 8) return 6;
  if(droppedFrames <= 10) return 7;
  return 8;
}

}

auto CPU::Profiler::power(bool) -> void {
  if(active) endCapture(cycles());
  isConfigured = false;
  shutdownRequested = false;
  pendingLevelStart = false;
  replayActive = false;
  replayRequested = false;
  externalReplay = false;
  replayRunning = false;
  replayFinished = false;
  replayQuit = false;
  replayHasFrameSeeds = false;
  replayFrameIndex = 0;
  replayInitialRandomSeed = 0;
  replayInitialChrObjRandomSeed = 0;
  gameFrameActive = false;
  functions.clear();
  replayHooks.clear();
  objects.clear();
  replayFrames.clear();
  functionStarts.clear();
  functionCache.clear();
  stageLoadFunction = NoFunction;
  stageUnloadFunction = NoFunction;
  replayStageLoadFunction = NoFunction;
  replayStopFunction = NoFunction;
  masterDisplayListFunction = NoFunction;
  debugMenuDrawFunction = NoFunction;
  softwareTlbLoadFunction = NoFunction;
  bossMainloopFunction = NoFunction;
  updateFrameCountersFunction = NoFunction;
  joyConsumeSamplesFunction = NoFunction;
  getControlTypeFunction = NoFunction;
  setSelectedDifficultyFunction = NoFunction;
  lvlSetSelectedDifficultyFunction = NoFunction;
  setControlTypeFunction = NoFunction;
  setInvertLookFunction = NoFunction;
  setAutoAimFunction = NoFunction;
  setAimControlFunction = NoFunction;
  setSightFunction = NoFunction;
  setLookAheadFunction = NoFunction;
  setAmmoFunction = NoFunction;
  setScreenFunction = NoFunction;
  setRatioFunction = NoFunction;

  auto symbols = std::getenv("ARES_N64_PROFILE_SYMBOLS");
  if(!symbols || !*symbols) return;
  symbolsPath = symbols;

  auto output = std::getenv("ARES_N64_PROFILE_OUTPUT");
  outputPrefix = output && *output ? output : "ares-n64-profile";
  auto replay = std::getenv("ARES_N64_PROFILE_REPLAY");
  auto externalReplayPath = std::getenv("ARES_N64_REPLAY");
  externalReplay = externalReplayPath && *externalReplayPath;
  if(externalReplay) replayPath = externalReplayPath;
  replayRequested = externalReplay ||
    (replay && *replay && std::string_view(replay) != "0");
  auto quit = std::getenv("ARES_N64_REPLAY_QUIT");
  replayQuit = quit && *quit && std::string_view(quit) != "0";

  if(!loadSymbols(symbolsPath)) {
    std::fprintf(stderr, "ares N64 profiler: could not load ELF symbols from %s\n", symbolsPath.c_str());
    return;
  }

  replayHooks.resize(functions.size(), ReplayHook::None);
  size_t optionGetterCount = 0;
  for(size_t index = 0; index < functions.size(); index++) {
    if(functions[index].name == "lvlStageLoad") stageLoadFunction = index;
    if(functions[index].name == "lvlUnloadStageTextData") stageUnloadFunction = index;
    if(functions[index].name == "practice_replay_on_stage_load") replayStageLoadFunction = index;
    if(functions[index].name == "practice_replay_stop_playback") replayStopFunction = index;
    if(functions[index].name == "dynGetMasterDisplayList") masterDisplayListFunction = index;
    if(functions[index].name == "debmenuDraw") debugMenuDrawFunction = index;
    if(functions[index].name == "tlbmanageTranslateLoadRomFromTlbAddress") softwareTlbLoadFunction = index;
    if(functions[index].name == "bossMainloop") bossMainloopFunction = index;
    if(functions[index].name == "updateFrameCounters") updateFrameCountersFunction = index;
    if(functions[index].name == "joyConsumeSamplesWrapper") joyConsumeSamplesFunction = index;
    if(functions[index].name == "cur_player_get_control_type") getControlTypeFunction = index;
    if(functions[index].name == "get_cur_player_look_vertical_inverted") {
      replayHooks[index] = ReplayHook::GetInvertLook;
      optionGetterCount++;
    }
    if(functions[index].name == "cur_player_get_autoaim") {
      replayHooks[index] = ReplayHook::GetAutoAim;
      optionGetterCount++;
    }
    if(functions[index].name == "cur_player_get_aim_control") {
      replayHooks[index] = ReplayHook::GetAimControl;
      optionGetterCount++;
    }
    if(functions[index].name == "cur_player_get_sight_onscreen_control") {
      replayHooks[index] = ReplayHook::GetSight;
      optionGetterCount++;
    }
    if(functions[index].name == "cur_player_get_lookahead") {
      replayHooks[index] = ReplayHook::GetLookAhead;
      optionGetterCount++;
    }
    if(functions[index].name == "cur_player_get_ammo_onscreen_setting") {
      replayHooks[index] = ReplayHook::GetAmmo;
      optionGetterCount++;
    }
    if(functions[index].name == "cur_player_get_screen_setting") {
      replayHooks[index] = ReplayHook::GetScreen;
      optionGetterCount++;
    }
    if(functions[index].name == "get_screen_ratio") {
      replayHooks[index] = ReplayHook::GetRatio;
      optionGetterCount++;
    }
    if(functions[index].name == "set_selected_difficulty") setSelectedDifficultyFunction = index;
    if(functions[index].name == "lvlSetSelectedDifficulty") lvlSetSelectedDifficultyFunction = index;
    if(functions[index].name == "cur_player_set_control_type") setControlTypeFunction = index;
    if(functions[index].name == "set_cur_player_look_vertical_inverted") setInvertLookFunction = index;
    if(functions[index].name == "cur_player_set_autoaim") setAutoAimFunction = index;
    if(functions[index].name == "cur_player_set_aim_control") setAimControlFunction = index;
    if(functions[index].name == "cur_player_set_sight_onscreen_control") setSightFunction = index;
    if(functions[index].name == "cur_player_set_lookahead") setLookAheadFunction = index;
    if(functions[index].name == "cur_player_set_ammo_onscreen_setting") setAmmoFunction = index;
    if(functions[index].name == "cur_player_set_screen_setting") setScreenFunction = index;
    if(functions[index].name == "set_screen_ratio") setRatioFunction = index;
  }

  auto setReplayHook = [&](size_t function, ReplayHook hook) {
    if(function != NoFunction) replayHooks[function] = hook;
  };
  setReplayHook(bossMainloopFunction, ReplayHook::BossMainloop);
  setReplayHook(stageLoadFunction, ReplayHook::StageLoad);
  setReplayHook(setSelectedDifficultyFunction, ReplayHook::Difficulty);
  setReplayHook(lvlSetSelectedDifficultyFunction, ReplayHook::Difficulty);
  setReplayHook(getControlTypeFunction, ReplayHook::GetControlType);
  setReplayHook(setControlTypeFunction, ReplayHook::SetControlType);
  setReplayHook(setInvertLookFunction, ReplayHook::SetInvertLook);
  setReplayHook(setAutoAimFunction, ReplayHook::SetAutoAim);
  setReplayHook(setAimControlFunction, ReplayHook::SetAimControl);
  setReplayHook(setSightFunction, ReplayHook::SetSight);
  setReplayHook(setLookAheadFunction, ReplayHook::SetLookAhead);
  setReplayHook(setAmmoFunction, ReplayHook::SetAmmo);
  setReplayHook(setScreenFunction, ReplayHook::SetScreen);
  setReplayHook(setRatioFunction, ReplayHook::SetRatio);
  setReplayHook(updateFrameCountersFunction, ReplayHook::UpdateFrameCounters);
  if(stageLoadFunction == NoFunction || stageUnloadFunction == NoFunction) {
    std::fprintf(stderr, "ares N64 profiler: ELF is missing lvlStageLoad or lvlUnloadStageTextData\n");
    return;
  }

  if(externalReplay) {
    stageNumAddress = objectAddress("g_StageNum");
    selectedDifficultyAddress = objectAddress("selected_difficulty");
    levelDifficultyAddress = objectAddress("g_SelectedDifficulty");
    currentPlayerAddress = objectAddress("g_CurrentPlayer");
    contDataAddress = objectAddress("g_ContData");
    randomSeedAddress = objectAddress("g_randomSeed");
    chrObjRandomSeedAddress = objectAddress("g_chrObjRandomSeed");
    if(bossMainloopFunction == NoFunction ||
       masterDisplayListFunction == NoFunction ||
       debugMenuDrawFunction == NoFunction ||
       updateFrameCountersFunction == NoFunction ||
       joyConsumeSamplesFunction == NoFunction ||
       getControlTypeFunction == NoFunction ||
       optionGetterCount != 8 ||
       !stageNumAddress || !selectedDifficultyAddress ||
       !levelDifficultyAddress || !currentPlayerAddress || !contDataAddress ||
       !randomSeedAddress || !chrObjRandomSeedAddress) {
      std::fprintf(stderr, "TEST_FAILED ELF is missing required GoldenEye replay symbols\n");
      if(replayQuit) requestShutdown();
      return;
    }
    if(!loadReplay(replayPath)) {
      std::fprintf(stderr, "TEST_FAILED could not load replay from %s\n", replayPath.c_str());
      if(replayQuit) requestShutdown();
      return;
    }
  }

  functionCache.resize(FunctionCacheSize);
  isConfigured = true;
  configuredAt = std::chrono::steady_clock::now();
  std::fprintf(stderr, "ares N64 profiler: loaded %zu functions from %s\n",
               functions.size(), symbolsPath.c_str());
}

auto CPU::Profiler::unload() -> void {
  if(active) endCapture(cycles());
  isConfigured = false;
}

auto CPU::Profiler::loadSymbols(const std::string& path) -> bool {
  std::ifstream input(path, std::ios::binary);
  if(!input) return false;
  std::vector<u8> data((std::istreambuf_iterator<char>(input)), {});
  if(data.size() < 52) return false;
  if(data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') return false;
  if(data[4] != 1 || data[5] != 2) return false;  //ELF32, big-endian

  auto sectionOffset = readBigEndian32(data, 32);
  auto sectionEntrySize = readBigEndian16(data, 46);
  auto sectionCount = readBigEndian16(data, 48);
  if(sectionEntrySize < 40 || sectionOffset + u64(sectionEntrySize) * sectionCount > data.size()) return false;

  for(u32 sectionIndex = 0; sectionIndex < sectionCount; sectionIndex++) {
    auto section = sectionOffset + size_t(sectionIndex) * sectionEntrySize;
    if(readBigEndian32(data, section + 4) != 2) continue;  //SHT_SYMTAB
    auto symbolsOffset = readBigEndian32(data, section + 16);
    auto symbolsSize = readBigEndian32(data, section + 20);
    auto stringsIndex = readBigEndian32(data, section + 24);
    auto symbolSize = readBigEndian32(data, section + 36);
    if(symbolSize < 16 || stringsIndex >= sectionCount) continue;

    auto stringsSection = sectionOffset + size_t(stringsIndex) * sectionEntrySize;
    auto stringsOffset = readBigEndian32(data, stringsSection + 16);
    auto stringsSize = readBigEndian32(data, stringsSection + 20);
    if(u64(symbolsOffset) + symbolsSize > data.size()) continue;
    if(u64(stringsOffset) + stringsSize > data.size()) continue;

    for(size_t symbol = symbolsOffset; symbol + symbolSize <= u64(symbolsOffset) + symbolsSize; symbol += symbolSize) {
      auto nameOffset = readBigEndian32(data, symbol + 0);
      auto address = readBigEndian32(data, symbol + 4);
      auto size = readBigEndian32(data, symbol + 8);
      auto info = data[symbol + 12];
      auto type = info & 15;
      if((type != 1 && type != 2) || !size || nameOffset >= stringsSize) continue;
      auto name = symbolName(data, stringsOffset + nameOffset, stringsOffset + stringsSize);
      if(name.empty() || name[0] == '$' || name.rfind(".L", 0) == 0) continue;
      if(type == 1) {
        objects.insert_or_assign(name, Object{address, size});
      } else {
        functions.push_back({address, size, std::move(name)});
      }
    }
  }

  std::sort(functions.begin(), functions.end(), [](const Function& a, const Function& b) {
    if(a.address != b.address) return a.address < b.address;
    if(a.size != b.size) return a.size > b.size;
    return a.name < b.name;
  });
  functions.erase(std::unique(functions.begin(), functions.end(), [](const Function& a, const Function& b) {
    return a.address == b.address && a.name == b.name;
  }), functions.end());
  for(size_t index = 0; index < functions.size(); index++) {
    functionStarts.try_emplace(functions[index].address, index);
  }
  return !functions.empty();
}

auto CPU::Profiler::objectAddress(const char* name) const -> u32 {
  auto found = objects.find(name);
  return found == objects.end() ? 0 : found->second.address;
}

auto CPU::Profiler::guestAddress(u32 address) -> u64 {
  return u64(s64(s32(address)));
}

auto CPU::Profiler::loadReplay(const std::string& path) -> bool {
  static constexpr size_t SramOffset = 0x600;
  static constexpr u32 ReplayMagic = 0x47455250;
  static constexpr u16 ReplayVersion = 1;
  static constexpr u8 FrameSeeds = 1;

  std::ifstream input(path, std::ios::binary);
  if(!input) return false;
  std::vector<u8> data((std::istreambuf_iterator<char>(input)), {});
  if(data.size() < SramOffset + 48) return false;
  auto base = SramOffset;
  if(readBigEndian32(data, base) != ReplayMagic ||
     readBigEndian16(data, base + 4) != ReplayVersion) return false;

  auto headerSize = readBigEndian16(data, base + 6);
  auto totalSize = readBigEndian32(data, base + 8);
  auto frameCount = readBigEndian32(data, base + 12);
  replayStage = data[base + 17];
  replayDifficulty = data[base + 18];
  auto flags = data[base + 19];
  replayHasFrameSeeds = flags & FrameSeeds;
  replayInitialRandomSeed = readBigEndian64(data, base + 24);
  replayInitialChrObjRandomSeed = readBigEndian64(data, base + 32);
  replayDuration = readBigEndian32(data, base + 40);
  if(headerSize < 48 || !frameCount || totalSize < headerSize ||
     u64(base) + totalSize > data.size()) return false;

  size_t position = base + headerSize;
  auto limit = base + totalSize;
  u16 options = 0;
  replayFrames.reserve(frameCount);
  while(replayFrames.size() < frameCount && position < limit) {
    auto delta = data[position++];
    if(delta == 0) {
      if(position + 2 > limit) return false;
      options = readBigEndian16(data, position);
      position += 2;
      continue;
    }

    ReplayFrameData frame;
    frame.deltaFrames = delta;
    frame.options = options;
    if(replayHasFrameSeeds) {
      if(position + 16 > limit) return false;
      frame.randomSeed = readBigEndian64(data, position);
      frame.chrObjRandomSeed = readBigEndian64(data, position + 8);
      position += 16;
    }
    if(position + 4 > limit) return false;
    frame.buttons = readBigEndian16(data, position);
    frame.stickX = s8(data[position + 2]);
    frame.stickY = s8(data[position + 3]);
    position += 4;
    replayFrames.push_back(frame);
  }

  if(replayFrames.size() != frameCount) return false;
  std::fprintf(stderr,
    "ares N64 replay: loaded %zu frames for stage %u from %s\n",
    replayFrames.size(), replayStage, path.c_str());
  return true;
}

auto CPU::Profiler::startReplay(u64 now) -> void {
  if(!externalReplay || replayRunning || replayFinished) return;
  replayRunning = true;
  replayActive = true;
  replayFrameIndex = 0;
  auto& first = replayFrames.front();
  lastReplayFrameAt = std::chrono::steady_clock::now();
  lastReplayStatusAt = lastReplayFrameAt;

  // A clean single-player ROM may not call the control-type setter because
  // no save profile was loaded. Apply the recorded scheme to the live player
  // selected through its ELF symbol. These player field offsets are shared
  // by the retail US, JP and EU layouts.
  auto player = self.readDebug<Word>(guestAddress(currentPlayerAddress));
  if(player) {
    auto controlType = u32(first.options >> 8 & 7);
    self.writeDebug<Word>(guestAddress(player + 0x2a58), controlType);
    self.writeDebug<Word>(guestAddress(player + 0x2a5c), controlType);
  }

  // The regular controller ring normally has a negative playback controller
  // count and therefore depends on a physically connected controller. External
  // replay supplies that ring itself, so expose player 1 exactly as GoldenEye's
  // native playback ring does. Otherwise a headless Input/Driver=None run
  // returns zero from joyGetButtons/joyGetStick despite the queued samples.
  self.writeDebug<Word>(guestAddress(contDataAddress + 0x1f8), 1);

  std::fprintf(stderr, "REPLAY_STARTED frames=%zu duration=%u\n",
               replayFrames.size(), replayDuration);
}

auto CPU::Profiler::failReplay(const std::string& reason, u64 now) -> void {
  if(replayFinished) return;
  replayRunning = false;
  replayActive = false;
  replayFinished = true;
  std::fprintf(stderr, "TEST_FAILED %s\n", reason.c_str());
  if(active) {
    endCapture(now, replayQuit);
  } else if(replayQuit) {
    requestShutdown();
  }
}

auto CPU::Profiler::completeReplay(u64 now) -> void {
  if(replayFinished) return;
  replayRunning = false;
  replayActive = false;
  replayFinished = true;
  std::fprintf(stderr, "TEST_COMPLETE frames=%zu duration=%u\n",
               replayFrames.size(), replayDuration);
  if(active) {
    endCapture(now, replayQuit);
  } else if(replayQuit) {
    requestShutdown();
  }
}

auto CPU::Profiler::replayTick(u64 now) -> void {
  if(!replayRunning || replayFinished) return;
  if(replayFrameIndex >= replayFrames.size()) {
    completeReplay(now);
    return;
  }

  auto& frame = replayFrames[replayFrameIndex];
  if(replayHasFrameSeeds) {
    auto randomSeed = self.readDebug<Dual>(guestAddress(randomSeedAddress));
    auto chrObjRandomSeed = self.readDebug<Dual>(guestAddress(chrObjRandomSeedAddress));
    if(randomSeed != frame.randomSeed ||
       chrObjRandomSeed != frame.chrObjRandomSeed) {
      std::ostringstream reason;
      reason << "replay diverged frame=" << replayFrameIndex
             << std::hex
             << " randomSeed=" << randomSeed << "/" << frame.randomSeed
             << " chrObjRandomSeed=" << chrObjRandomSeed << "/"
             << frame.chrObjRandomSeed;
      failReplay(reason.str(), now);
      return;
    }
  }

  self.ipu.r[4].u64 = frame.deltaFrames;
}

auto CPU::Profiler::replayQueueInput() -> void {
  if(!replayRunning || replayFinished ||
     replayFrameIndex >= replayFrames.size()) return;

  // Apply exactly one replay sample after GoldenEye finishes consuming the
  // asynchronously-polled regular-controller ring. Injecting at the wrapper's
  // entry races the SI polling thread, which can advance nextlast before
  // joyConsumeSamples reads it and make gameplay consume a host input instead.
  auto current = u32(self.readDebug<Word>(guestAddress(contDataAddress + 0x1e0)));
  if(current >= 20) return;
  auto next = (current + 1) % 20;
  auto sample = contDataAddress + next * 24;
  auto previous = contDataAddress + current * 24;
  auto& expected = replayFrames[replayFrameIndex];
  auto previousButtons = u16(self.readDebug<Half>(guestAddress(previous)));
  self.writeDebug<Dual>(guestAddress(sample), 0);
  self.writeDebug<Dual>(guestAddress(sample + 8), 0);
  self.writeDebug<Dual>(guestAddress(sample + 16), 0);
  self.writeDebug<Half>(guestAddress(sample), expected.buttons);
  self.writeDebug<Byte>(guestAddress(sample + 2), u8(expected.stickX));
  self.writeDebug<Byte>(guestAddress(sample + 3), u8(expected.stickY));
  self.writeDebug<Byte>(guestAddress(sample + 4), 0);
  self.writeDebug<Word>(guestAddress(contDataAddress + 0x1e4), current);
  self.writeDebug<Word>(guestAddress(contDataAddress + 0x1e0), next);
  self.writeDebug<Word>(guestAddress(contDataAddress + 0x1e8), next);
  self.writeDebug<Word>(guestAddress(contDataAddress + 0x1ec), current);
  self.writeDebug<Dual>(guestAddress(contDataAddress + 0x1f0), 0);
  self.writeDebug<Half>(guestAddress(contDataAddress + 0x1f0),
                        expected.buttons & ~previousButtons);

  replayFrameIndex++;
}

auto CPU::Profiler::functionAt(u32 address) -> size_t {
  auto& cached = functionCache[address >> 2 & (FunctionCacheSize - 1)];
  if(cached.address == address && cached.function != ~u32{0}) {
    return cached.function == ~u32{1} ? NoFunction : cached.function;
  }

  size_t result = NoFunction;
  if(functions.empty()) return NoFunction;
  auto iterator = std::upper_bound(functions.begin(), functions.end(), address,
    [](u32 address, const Function& function) { return address < function.address; });
  while(iterator != functions.begin()) {
    --iterator;
    if(address >= iterator->address && u64(address) < u64(iterator->address) + iterator->size) {
      result = size_t(iterator - functions.begin());
      break;
    }
    if(iterator->address != address) break;
  }
  cached.address = address;
  cached.function = result == NoFunction ? ~u32{1} : u32(result);
  return result;
}

auto CPU::Profiler::functionStartingAt(u32 address) const -> size_t {
  auto found = functionStarts.find(address);
  return found == functionStarts.end() ? NoFunction : found->second;
}

auto CPU::Profiler::cycles() const -> u64 {
  auto pending = self.clock > 0 ? u64(self.clock >> 1) : 0;
  return u64(self.profile.cpuCycles) + pending;
}

auto CPU::Profiler::resetCapture() -> void {
  for(auto& function : functions) {
    function.calls = 0;
    function.selfCycles = 0;
    function.inclusiveCycles = 0;
  }
  stack.clear();
  pages.clear();
  frames.clear();
  gameFrames.clear();
  folded.clear();
  foldedKey.clear();
  foldedPendingCycles = 0;
  tlbCacheHits = 0;
  tlbCacheMisses = 0;
  tlbMissing = 0;
  for(auto& count : droppedFrameHistogram) count = 0;
  droppedFramePending = false;
  gameFrameHasDroppedFrame = false;
  lastFunction = NoFunction;
  frameStartCycle = 0;
  gameFrameStartCycle = 0;
  gameFrameTlbLoads = 0;
  gameFrameActive = false;
  replayActive = replayRequested;
  pendingReplayStartReturn = 0;
  pendingReplayOptionReturn = 0;
  pendingCall = false;
}

auto CPU::Profiler::beginCapture(u32 stage, u64 now) -> void {
  resetCapture();
  active = true;
  captureStage = stage;
  captureStartCycle = captureEndCycle = lastCycle = now;
  captureStartedAt = std::chrono::steady_clock::now();
  captureSequence++;
  std::fprintf(stderr, "ares N64 profiler: capture %u started for stage %u\n",
               captureSequence, captureStage);
}

auto CPU::Profiler::endCapture(u64 now, bool requestShutdownAfterWrite) -> void {
  if(!active) return;
  attributeUntil(now);
  flushFolded();
  frameStartCycle = 0;
  while(!stack.empty()) popFunction(now);
  captureEndCycle = now;
  active = false;
  writeCapture();
  if(requestShutdownAfterWrite) requestShutdown();
}

auto CPU::Profiler::checkTimeout(u64 now) -> bool {
  using namespace std::chrono_literals;
  if(externalReplay) {
    if(replayFinished) return false;
    auto wallNow = std::chrono::steady_clock::now();
    if(replayRunning) {
      if(wallNow - lastReplayFrameAt >= 20s) {
        failReplay("next replay frame was not rendered within 20 seconds", now);
        return replayQuit;
      }
      if(wallNow - lastReplayStatusAt >= 20s) {
        std::fprintf(stderr, "REPLAY_STATUS frame=%zu/%zu\n",
                     replayFrameIndex, replayFrames.size());
        lastReplayStatusAt = wallNow;
      }
    } else {
      auto elapsed = std::chrono::duration<double>(wallNow - configuredAt).count();
      if(elapsed >= 20.0) {
        failReplay("level did not start before timeout", now);
        return replayQuit;
      }
    }
    return false;
  }

  auto elapsed = std::chrono::steady_clock::now() - (active ? captureStartedAt : configuredAt);
  if(!active && elapsed >= 60s) {
    std::fprintf(stderr, "ares N64 profiler: timed out waiting 60 seconds for capture to start\n");
    requestShutdown();
    return true;
  }
  if(active && elapsed >= 30min) {
    std::fprintf(stderr, "ares N64 profiler: timed out waiting 30 minutes for capture to finish; writing partial capture\n");
    endCapture(now, true);
    return true;
  }
  return false;
}

auto CPU::Profiler::requestShutdown() -> void {
  if(shutdownRequested) return;
  shutdownRequested = true;
  if(platform) platform->event(ares::Event::Shutdown);
}

auto CPU::Profiler::attributeUntil(u64 now) -> void {
  if(!active || now < lastCycle) {
    lastCycle = now;
    return;
  }
  auto delta = now - lastCycle;
  if(lastFunction != NoFunction) functions[lastFunction].selfCycles += delta;
  foldedPendingCycles += delta;
  lastCycle = now;
}

auto CPU::Profiler::flushFolded() -> void {
  if(foldedPendingCycles && !foldedKey.empty()) folded[foldedKey] += foldedPendingCycles;
  foldedPendingCycles = 0;
}

auto CPU::Profiler::updateFoldedKey() -> void {
  foldedKey.clear();
  for(auto& entry : stack) {
    if(!foldedKey.empty()) foldedKey += ';';
    foldedKey += functions[entry.function].name;
  }
  if(foldedKey.empty() && lastFunction != NoFunction) foldedKey = functions[lastFunction].name;
}

auto CPU::Profiler::pushFunction(size_t function, u32 returnAddress, u64 now) -> void {
  if(function == NoFunction) return;

  // If a call site is reached again while its previous frame is still on the
  // recovered stack, a return boundary was missed (usually across a JIT or
  // scheduler transition). Discard that stale frame and anything above it.
  for(size_t depth = stack.size(); depth; depth--) {
    auto& entry = stack[depth - 1];
    if(entry.function != function || entry.returnAddress != returnAddress) continue;
    while(stack.size() >= depth) popFunction(now);
    break;
  }

  flushFolded();
  stack.push_back({function, returnAddress, u32(self.ipu.r[29].u64), now});
  functions[function].calls++;
  updateFoldedKey();
}

auto CPU::Profiler::popFunction(u64 now) -> void {
  if(stack.empty()) return;
  flushFolded();
  auto entry = stack.back();
  stack.pop_back();
  if(now >= entry.startCycle) functions[entry.function].inclusiveCycles += now - entry.startCycle;
  updateFoldedKey();
}

auto CPU::Profiler::instruction(u64 address_, u32 instruction_) -> void {
  if(!isConfigured) return;
  auto address = u32(address_);
  auto now = cycles();
  auto currentFunction = functionAt(address);
  auto exactFunction = functionStartingAt(address);
  auto inFunction = [&](size_t function) {
    if(function == NoFunction) return false;
    auto& symbol = functions[function];
    return address >= symbol.address && u64(address) < u64(symbol.address) + symbol.size;
  };
  auto inReplayStageLoad = inFunction(replayStageLoadFunction);
  auto inReplayStop = inFunction(replayStopFunction);

  if(active && replayActive && exactFunction == updateFrameCountersFunction) {
    pendingDroppedFrameBucket = u8(droppedFrameBucket(self.ipu.r[4].u64));
    droppedFramePending = true;
  }

  if(externalReplay && replayRunning && !replayFinished &&
     currentFunction == joyConsumeSamplesFunction &&
     instruction_ == 0x03e0'0008u) {
    replayQueueInput();
  }

  if(externalReplay && !replayFinished) {
    if(pendingReplayOptionReturn && address == pendingReplayOptionReturn) {
      self.ipu.r[2].u64 = pendingReplayOptionValue;
      pendingReplayOptionReturn = 0;
    }
    if(exactFunction != NoFunction) {
      auto hook = replayHooks[exactFunction];
      if(hook != ReplayHook::None) {
        auto options = replayFrames.empty() ? 0 : replayFrames[
          std::min(replayFrameIndex, replayFrames.size() - 1)].options;
        switch(hook) {
        case ReplayHook::BossMainloop:
          self.writeDebug<Word>(guestAddress(stageNumAddress), replayStage);
          self.writeDebug<Word>(guestAddress(selectedDifficultyAddress),
                                replayDifficulty);
          self.writeDebug<Word>(guestAddress(levelDifficultyAddress),
                                replayDifficulty);
          break;
        case ReplayHook::StageLoad:
          self.ipu.r[4].u64 = replayStage;
          self.writeDebug<Word>(guestAddress(selectedDifficultyAddress),
                                replayDifficulty);
          self.writeDebug<Word>(guestAddress(levelDifficultyAddress),
                                replayDifficulty);
          self.writeDebug<Dual>(guestAddress(randomSeedAddress),
                                replayInitialRandomSeed);
          self.writeDebug<Dual>(guestAddress(chrObjRandomSeedAddress),
                                replayInitialChrObjRandomSeed);
          break;
        case ReplayHook::Difficulty:
          self.ipu.r[4].u64 = replayDifficulty;
          break;
        case ReplayHook::GetControlType:
          pendingReplayOptionReturn = u32(self.ipu.r[31].u64);
          pendingReplayOptionValue = options >> 8 & 7;
          break;
        case ReplayHook::GetInvertLook:
          pendingReplayOptionReturn = u32(self.ipu.r[31].u64);
          pendingReplayOptionValue = (options & 0x0001) != 0;
          break;
        case ReplayHook::GetAutoAim:
          pendingReplayOptionReturn = u32(self.ipu.r[31].u64);
          pendingReplayOptionValue = (options & 0x0002) != 0;
          break;
        case ReplayHook::GetAimControl:
          pendingReplayOptionReturn = u32(self.ipu.r[31].u64);
          pendingReplayOptionValue = (options & 0x0004) != 0;
          break;
        case ReplayHook::GetSight:
          pendingReplayOptionReturn = u32(self.ipu.r[31].u64);
          pendingReplayOptionValue = (options & 0x0008) != 0;
          break;
        case ReplayHook::GetLookAhead:
          pendingReplayOptionReturn = u32(self.ipu.r[31].u64);
          pendingReplayOptionValue = (options & 0x0010) != 0;
          break;
        case ReplayHook::GetAmmo:
          pendingReplayOptionReturn = u32(self.ipu.r[31].u64);
          pendingReplayOptionValue = (options & 0x0020) != 0;
          break;
        case ReplayHook::GetScreen:
          pendingReplayOptionReturn = u32(self.ipu.r[31].u64);
          pendingReplayOptionValue =
            options & 0x0800 ? 2 : options & 0x0040 ? 1 : 0;
          break;
        case ReplayHook::GetRatio:
          pendingReplayOptionReturn = u32(self.ipu.r[31].u64);
          pendingReplayOptionValue = (options & 0x0080) != 0;
          break;
        case ReplayHook::SetControlType:
          self.ipu.r[4].u64 = options >> 8 & 7;
          break;
        case ReplayHook::SetInvertLook:
          self.ipu.r[4].u64 = (options & 0x0001) != 0;
          break;
        case ReplayHook::SetAutoAim:
          self.ipu.r[4].u64 = (options & 0x0002) != 0;
          break;
        case ReplayHook::SetAimControl:
          self.ipu.r[4].u64 = (options & 0x0004) != 0;
          break;
        case ReplayHook::SetSight:
          self.ipu.r[4].u64 = (options & 0x0008) != 0;
          break;
        case ReplayHook::SetLookAhead:
          self.ipu.r[4].u64 = (options & 0x0010) != 0;
          break;
        case ReplayHook::SetAmmo:
          self.ipu.r[4].u64 = (options & 0x0020) != 0;
          break;
        case ReplayHook::SetScreen:
          self.ipu.r[4].u64 =
            options & 0x0800 ? 2 : options & 0x0040 ? 1 : 0;
          break;
        case ReplayHook::SetRatio:
          self.ipu.r[4].u64 = (options & 0x0080) != 0;
          break;
        case ReplayHook::UpdateFrameCounters:
          if(replayRunning) {
            replayTick(now);
            if(replayFinished) return;
          }
          break;
        case ReplayHook::None:
          break;
        }
      }
    }
  }

  if(pendingLevelStart && address == pendingLevelReturn) {
    pendingLevelStart = false;
    beginCapture(pendingLevelStage, now);
    startReplay(now);
  }

  if(exactFunction == stageLoadFunction) {
    pendingLevelStage = u32(self.ipu.r[4].u64);
    if(active) endCapture(now, pendingLevelStage == GoldenEyeTitleStage);
    pendingLevelReturn = u32(self.ipu.r[31].u64);
    pendingLevelStart = pendingLevelStage != GoldenEyeTitleStage;
    if(shutdownRequested) return;
  }
  // Start at lvlStageLoad's return instruction. Waiting only for the caller's
  // return address is unreliable when execution crosses JIT/scheduler blocks.
  if(pendingLevelStart && currentFunction == stageLoadFunction && instruction_ == 0x03e0'0008u) {
    pendingLevelStart = false;
    beginCapture(pendingLevelStage, now);
    startReplay(now);
  }
  if(exactFunction == stageUnloadFunction && active) {
    if(externalReplay && replayRunning) {
      failReplay("level ended before replay finished", now);
      return;
    }
    endCapture(now, true);
    return;
  }
  if(replayRequested && !active && inReplayStageLoad) {
    beginCapture(0, now);
  }
  if(replayRequested && active && inReplayStop) {
    endCapture(now, true);
    return;
  }
  if(!active) return;

  if(pendingReplayStartReturn && address == pendingReplayStartReturn) {
    pendingReplayStartReturn = 0;
    replayActive = true;
  }
  if(inReplayStageLoad && !pendingReplayStartReturn) {
    pendingReplayStartReturn = u32(self.ipu.r[31].u64);
  }
  if(inReplayStop) {
    replayActive = false;
    gameFrameActive = false;
    droppedFramePending = false;
    gameFrameHasDroppedFrame = false;
  }
  // The JIT can skip the caller's return PC, but it consistently exposes each
  // callee's JR RA. These bracket the same work as the guest-side profiler:
  // after dynGetMasterDisplayList through the end of debmenuDraw.
  if(replayActive && currentFunction == masterDisplayListFunction
      && instruction_ == 0x03e0'0008u) {
    gameFrameActive = true;
    gameFrameStartCycle = now;
    gameFrameTlbLoads = 0;
    gameFrameHasDroppedFrame = droppedFramePending;
    gameFrameDroppedFrameBucket = pendingDroppedFrameBucket;
    droppedFramePending = false;
  }
  if(gameFrameActive && currentFunction == debugMenuDrawFunction
      && instruction_ == 0x03e0'0008u && now >= gameFrameStartCycle) {
    gameFrames.push_back({gameFrameStartCycle, now, gameFrameTlbLoads});
    if(gameFrameHasDroppedFrame) {
      droppedFrameHistogram[gameFrameDroppedFrameBucket]++;
    }
    gameFrameActive = false;
    gameFrameHasDroppedFrame = false;
    if(externalReplay && replayRunning) {
      lastReplayFrameAt = std::chrono::steady_clock::now();
      if(replayFrameIndex >= replayFrames.size()) {
        completeReplay(now);
        return;
      }
    }
  }
  if(exactFunction == softwareTlbLoadFunction && gameFrameActive) {
    gameFrameTlbLoads++;
  }

  attributeUntil(now);

  auto stackPointer = u32(self.ipu.r[29].u64);

  // The MIPS stack grows downward. If execution is back outside the current
  // function with SP restored to (or above) its entry value, its return was
  // crossed even if the exact return PC was hidden by a JIT/scheduler edge.
  // Do not apply this while a call and its delay slot are still pending: a
  // callee initially sees the caller's SP before running its own prologue.
  if(!pendingCall) {
    while(!stack.empty() && currentFunction != stack.back().function
        && stackPointer >= stack.back().stackPointer) {
      popFunction(now);
    }
  }

  while(!stack.empty() && stack.back().returnAddress && stack.back().returnAddress == address) {
    popFunction(now);
  }

  bool enteredCall = false;
  if(pendingCall && address != pendingCallDelay) {
    if(address == pendingCallTarget) {
      pushFunction(currentFunction, pendingCallReturn, now);
      enteredCall = true;
    }
    pendingCall = false;
  }

  if(stack.empty()) {
    if(currentFunction != NoFunction) pushFunction(currentFunction, 0, now);
  } else if(!enteredCall && exactFunction != NoFunction && stack.back().function != exactFunction) {
    // Recover calls that crossed an unobserved JAL/JALR edge using SP and RA.
    // A restored/higher SP instead indicates a tail call or context switch.
    if(stackPointer < stack.back().stackPointer) {
      pushFunction(exactFunction, u32(self.ipu.r[31].u64), now);
    } else {
      while(!stack.empty()) popFunction(now);
      pushFunction(exactFunction, 0, now);
    }
  }

  lastFunction = currentFunction;

  auto opcode = instruction_ >> 26;
  if(opcode == 3) {  //JAL
    pendingCall = true;
    pendingCallDelay = address + 4;
    pendingCallTarget = (address & 0xf000'0000u) | (instruction_ & 0x03ff'ffffu) << 2;
    pendingCallReturn = address + 8;
  } else if(opcode == 0 && (instruction_ & 63) == 9) {  //JALR
    auto source = instruction_ >> 21 & 31;
    pendingCall = true;
    pendingCallDelay = address + 4;
    pendingCallTarget = u32(self.ipu.r[source].u64);
    pendingCallReturn = address + 8;
  }
}

auto CPU::Profiler::frame() -> void {
  if(isConfigured && checkTimeout(cycles())) return;
  if(!active) return;
  auto now = cycles();
  attributeUntil(now);
  if(frameStartCycle && now >= frameStartCycle) frames.push_back({frameStartCycle, now});
  frameStartCycle = now;
}

auto CPU::Profiler::tlbAccess(u64 address, bool store, TlbResult result) -> void {
  if(!active) return;
  auto pageAddress = u32(address) & ~0x1fffu;
  auto& page = pages[pageAddress];
  page.accesses++;
  store ? page.stores++ : page.loads++;
  if(result == TlbResult::CacheHit) {
    page.cacheHits++;
    tlbCacheHits++;
  } else {
    page.cacheMisses++;
    tlbCacheMisses++;
    if(result == TlbResult::Missing) {
      page.missing++;
      tlbMissing++;
    }
  }
}

auto CPU::Profiler::capturePath(const char* suffix) const -> std::filesystem::path {
  std::ostringstream name;
  name << outputPrefix.string() << '-' << std::setfill('0') << std::setw(3) << captureSequence << suffix;
  return name.str();
}

auto CPU::Profiler::csv(const std::string& value) -> std::string {
  std::string result = "\"";
  for(auto character : value) result += character == '"' ? "\"\"" : std::string(1, character);
  return result + '"';
}

auto CPU::Profiler::writeCapture() -> void {
  auto summaryPath = capturePath("-summary.csv");
  if(auto parent = summaryPath.parent_path(); !parent.empty()) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
  }

  u64 frameTotal = 0;
  for(auto& frame : frames) frameTotal += frame.endCycle - frame.startCycle;

  {
    std::ofstream output(summaryPath);
    output << "metric,value\n"
           << "ares_version," << csv((const char*)ares::Version) << "\n"
           << "profiler_version," << csv((const char*)ares::ProfilerVersion) << "\n"
           << "stage," << captureStage << "\n"
           << "start_cycle," << captureStartCycle << "\n"
           << "end_cycle," << captureEndCycle << "\n"
           << "total_cycles," << captureEndCycle - captureStartCycle << "\n"
           << "frames," << frames.size() << "\n"
           << "average_frame_delta," << (frames.empty() ? 0 : frameTotal / frames.size()) << "\n"
           << "tlb_cache_hits," << tlbCacheHits << "\n"
           << "tlb_cache_misses," << tlbCacheMisses << "\n"
           << "tlb_missing," << tlbMissing << "\n";
    for(size_t index = 0; index < std::size(DroppedFrameMetrics); index++) {
      output << DroppedFrameMetrics[index] << ',' << droppedFrameHistogram[index] << "\n";
    }
  }

  {
    std::ofstream output(capturePath("-functions.csv"));
    output << "address,size,name,calls,self_cycles,inclusive_cycles\n";
    for(auto& function : functions) {
      if(!function.calls && !function.selfCycles) continue;
      output << "0x" << std::hex << std::setw(8) << std::setfill('0') << function.address
             << std::dec << ',' << function.size << ',' << csv(function.name) << ','
             << function.calls << ',' << function.selfCycles << ',' << function.inclusiveCycles << "\n";
    }
  }

  {
    std::ofstream output(capturePath("-tlb.csv"));
    output << "page,accesses,loads,stores,cache_hits,cache_misses,missing\n";
    for(auto& [address, page] : pages) {
      output << "0x" << std::hex << std::setw(8) << std::setfill('0') << address << std::dec
             << ',' << page.accesses << ',' << page.loads << ',' << page.stores << ','
             << page.cacheHits << ',' << page.cacheMisses << ',' << page.missing << "\n";
    }
  }

  {
    std::ofstream output(capturePath("-frames.csv"));
    output << "frame,start_cycle,end_cycle,delta_cycles\n";
    for(size_t index = 0; index < frames.size(); index++) {
      auto& frame = frames[index];
      output << index << ',' << frame.startCycle << ',' << frame.endCycle << ','
             << frame.endCycle - frame.startCycle << "\n";
    }
  }

  {
    std::ofstream output(capturePath("-game-frames.csv"));
    output << "frame,tick_cycles,tlb_loads,start_cycle,end_cycle\n";
    for(size_t index = 0; index < gameFrames.size(); index++) {
      auto& frame = gameFrames[index];
      // VR4300 CP0 Count (used by osGetCount) advances once per two CPU cycles.
      output << index << ',' << (frame.endCycle - frame.startCycle) / 2 << ',' << frame.tlbLoads
             << ',' << frame.startCycle << ',' << frame.endCycle << "\n";
    }
  }

  {
    std::ofstream output(capturePath(".folded"));
    for(auto& [callstack, count] : folded) output << callstack << ' ' << count << "\n";
  }

  std::fprintf(stderr, "ares N64 profiler: capture %u written:\n", captureSequence);
  std::fprintf(stderr, "  %s\n", summaryPath.string().c_str());
  std::fprintf(stderr, "  %s\n", capturePath("-functions.csv").string().c_str());
  std::fprintf(stderr, "  %s\n", capturePath("-tlb.csv").string().c_str());
  std::fprintf(stderr, "  %s\n", capturePath("-frames.csv").string().c_str());
  std::fprintf(stderr, "  %s (%zu GoldenEye replay frames)\n",
               capturePath("-game-frames.csv").string().c_str(), gameFrames.size());
  std::fprintf(stderr, "  %s\n", capturePath(".folded").string().c_str());
}
