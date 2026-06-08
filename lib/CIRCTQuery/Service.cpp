#include "CIRCTQuery/Service.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

namespace circt_query {
namespace {

std::string joinPath(const std::string &prefix, const std::string &name) {
  return prefix.empty() ? name : prefix + "/" + name;
}

std::string normalizePath(std::string value) {
  std::replace(value.begin(), value.end(), '.', '/');
  return value;
}

bool wildcardMatch(const std::string &candidate, const std::string &pattern) {
  size_t candidateIndex = 0;
  size_t patternIndex = 0;
  size_t starPatternIndex = std::string::npos;
  size_t starCandidateIndex = 0;
  while (candidateIndex < candidate.size()) {
    if (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
      starPatternIndex = patternIndex++;
      starCandidateIndex = candidateIndex;
      continue;
    }
    if (patternIndex < pattern.size() &&
        pattern[patternIndex] == candidate[candidateIndex]) {
      ++patternIndex;
      ++candidateIndex;
      continue;
    }
    if (starPatternIndex == std::string::npos)
      return false;
    patternIndex = starPatternIndex + 1;
    candidateIndex = ++starCandidateIndex;
  }
  while (patternIndex < pattern.size() && pattern[patternIndex] == '*')
    ++patternIndex;
  return patternIndex == pattern.size();
}

std::optional<int> fuzzyScore(const std::string &candidate,
                              const std::string &query) {
  if (query.empty())
    return 0;
  std::string lowerCandidate = candidate;
  std::string lowerQuery = query;
  std::transform(lowerCandidate.begin(), lowerCandidate.end(),
                 lowerCandidate.begin(),
                 [](unsigned char ch) { return std::tolower(ch); });
  std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(),
                 [](unsigned char ch) { return std::tolower(ch); });

  size_t queryIndex = 0;
  int score = 0;
  int lastMatch = -1;
  for (size_t candidateIndex = 0; candidateIndex < lowerCandidate.size();
       ++candidateIndex) {
    if (queryIndex >= lowerQuery.size())
      break;
    if (lowerCandidate[candidateIndex] != lowerQuery[queryIndex])
      continue;
    score += lastMatch >= 0 ? static_cast<int>(candidateIndex) - lastMatch - 1
                            : static_cast<int>(candidateIndex);
    lastMatch = static_cast<int>(candidateIndex);
    ++queryIndex;
  }
  if (queryIndex != lowerQuery.size())
    return std::nullopt;
  score += static_cast<int>(candidate.size() - query.size());
  return score;
}

std::optional<int> matchScore(const std::vector<std::string> &terms,
                              const std::string &query, MatchMode mode) {
  if (query.empty())
    return 0;
  if (mode == MatchMode::Wildcard) {
    std::string normalizedQuery = normalizePath(query);
    for (const auto &term : terms) {
      if (wildcardMatch(normalizePath(term), normalizedQuery))
        return 0;
    }
    return std::nullopt;
  }
  if (mode == MatchMode::Regex) {
    std::regex regex(query, std::regex::ECMAScript);
    for (const auto &term : terms) {
      if (std::regex_match(term, regex) ||
          std::regex_match(normalizePath(term), regex))
        return 0;
    }
    return std::nullopt;
  }

  std::optional<int> best;
  for (const auto &term : terms) {
    if (auto score = fuzzyScore(normalizePath(term), normalizePath(query))) {
      if (!best || *score < *best)
        best = *score;
    }
    if (auto score = fuzzyScore(term, query)) {
      if (!best || *score < *best)
        best = *score;
    }
  }
  return best;
}

bool objectMatches(const DesignObjectInfo &object, const std::string &query,
                   MatchMode mode) {
  return matchScore({object.path, object.localName, object.moduleName,
                     object.targetModule},
                    query, mode)
      .has_value();
}

void appendObject(std::vector<DesignObjectInfo> *objects,
                  DesignObjectInfo object,
                  const std::optional<ObjectKind> &kind) {
  if (kind && object.kind != *kind)
    return;
  objects->push_back(std::move(object));
}

void collectModuleLocalObjects(const DesignRecord &record,
                               const ModuleInfo &module,
                               const std::string &pathPrefix,
                               const std::string &instancePath,
                               std::vector<DesignObjectInfo> *objects,
                               std::optional<ObjectKind> kind) {
  for (const auto &port : module.ports) {
    DesignObjectInfo object;
    object.path = joinPath(pathPrefix, port.name);
    object.kind = ObjectKind::Port;
    object.designId = record.id;
    object.moduleName = module.name;
    object.localName = port.name;
    object.instancePath = instancePath;
    object.type = port.type;
    object.direction = port.direction;
    appendObject(objects, std::move(object), kind);
  }
  for (const auto &reg : module.registers) {
    DesignObjectInfo object;
    object.path = joinPath(pathPrefix, reg.name);
    object.kind = ObjectKind::Register;
    object.designId = record.id;
    object.moduleName = module.name;
    object.localName = reg.name;
    object.instancePath = instancePath;
    object.type = reg.type;
    appendObject(objects, std::move(object), kind);
  }
  for (const auto &memoryPort : module.memoryPorts) {
    DesignObjectInfo object;
    object.path = joinPath(pathPrefix, memoryPort.name);
    object.kind = ObjectKind::MemoryPort;
    object.designId = record.id;
    object.moduleName = module.name;
    object.localName = memoryPort.name;
    object.instancePath = instancePath;
    object.type = memoryPort.type;
    appendObject(objects, std::move(object), kind);
  }
  for (const auto &instance : module.instances) {
    DesignObjectInfo object;
    object.path = joinPath(pathPrefix, instance.name);
    object.kind = ObjectKind::Instance;
    object.designId = record.id;
    object.moduleName = module.name;
    object.localName = instance.name;
    object.instancePath = instancePath;
    object.targetModule = instance.targetModule;
    appendObject(objects, std::move(object), kind);
  }
}

void collectHierarchyObjects(
    const DesignRecord &record, const DesignModel &model,
    const std::string &moduleName, const std::string &pathPrefix,
    const std::string &instancePath, std::set<std::string> *active,
    std::vector<DesignObjectInfo> *objects, std::optional<ObjectKind> kind) {
  auto moduleIt = model.modules.find(moduleName);
  if (moduleIt == model.modules.end())
    return;

  std::string recursionKey = pathPrefix + ":" + moduleName;
  if (!active->insert(recursionKey).second)
    return;

  const ModuleInfo &module = moduleIt->second;
  collectModuleLocalObjects(record, module, pathPrefix, instancePath, objects,
                            kind);
  for (const auto &instance : module.instances) {
    collectHierarchyObjects(record, model, instance.targetModule,
                            joinPath(pathPrefix, instance.name),
                            joinPath(instancePath, instance.name), active,
                            objects, kind);
  }
  active->erase(recursionKey);
}

std::string getObjectParentPath(const DesignObjectInfo &object) {
  std::string suffix = "/" + object.localName;
  if (object.path.size() >= suffix.size() &&
      object.path.compare(object.path.size() - suffix.size(), suffix.size(),
                          suffix) == 0) {
    return object.path.substr(0, object.path.size() - suffix.size());
  }
  return object.moduleName;
}

const PortInfo *findPort(const ModuleInfo &module, const std::string &name) {
  auto it =
      std::find_if(module.ports.begin(), module.ports.end(),
                   [&](const PortInfo &port) { return port.name == name; });
  return it == module.ports.end() ? nullptr : &*it;
}

const RegisterInfo *findRegisterForValue(const ModuleInfo &module,
                                         const std::string &valueName) {
  auto it = std::find_if(module.registers.begin(), module.registers.end(),
                         [&](const RegisterInfo &reg) {
                           return reg.name == valueName ||
                                  reg.outputValueName == valueName;
                         });
  return it == module.registers.end() ? nullptr : &*it;
}

const MemoryPortInfo *findMemoryPortForValue(const ModuleInfo &module,
                                             const std::string &valueName) {
  auto it = std::find_if(module.memoryPorts.begin(), module.memoryPorts.end(),
                         [&](const MemoryPortInfo &port) {
                           return port.name == valueName ||
                                  port.outputValueName == valueName;
                         });
  return it == module.memoryPorts.end() ? nullptr : &*it;
}

const InstanceInfo *findInstance(const ModuleInfo &module,
                                 const std::string &name) {
  auto it =
      std::find_if(module.instances.begin(), module.instances.end(),
                   [&](const InstanceInfo &inst) { return inst.name == name; });
  return it == module.instances.end() ? nullptr : &*it;
}

DesignObjectInfo makePortObject(const DesignRecord &record,
                                const std::string &pathPrefix,
                                const std::string &instancePath,
                                const ModuleInfo &module,
                                const PortInfo &port) {
  DesignObjectInfo object;
  object.path = joinPath(pathPrefix, port.name);
  object.kind = ObjectKind::Port;
  object.designId = record.id;
  object.moduleName = module.name;
  object.localName = port.name;
  object.instancePath = instancePath;
  object.type = port.type;
  object.direction = port.direction;
  return object;
}

DesignObjectInfo makeRegisterObject(const DesignRecord &record,
                                    const std::string &pathPrefix,
                                    const std::string &instancePath,
                                    const ModuleInfo &module,
                                    const RegisterInfo &reg) {
  DesignObjectInfo object;
  object.path = joinPath(pathPrefix, reg.name);
  object.kind = ObjectKind::Register;
  object.designId = record.id;
  object.moduleName = module.name;
  object.localName = reg.name;
  object.instancePath = instancePath;
  object.type = reg.type;
  return object;
}

DesignObjectInfo makeMemoryPortObject(const DesignRecord &record,
                                      const std::string &pathPrefix,
                                      const std::string &instancePath,
                                      const ModuleInfo &module,
                                      const MemoryPortInfo &port) {
  DesignObjectInfo object;
  object.path = joinPath(pathPrefix, port.name);
  object.kind = ObjectKind::MemoryPort;
  object.designId = record.id;
  object.moduleName = module.name;
  object.localName = port.name;
  object.instancePath = instancePath;
  object.type = port.type;
  return object;
}

ValueRef makeValueRef(const DesignRecord &record, const ModuleInfo &module,
                      const std::string &instancePath, const ValueInfo &value) {
  ValueRef ref;
  ref.designId = record.id;
  ref.moduleName = module.name;
  ref.instancePath = instancePath;
  ref.valueName = value.name;
  ref.type = value.type;
  ref.definingOp = value.definingOp;
  return ref;
}

ValueRef makeUnresolvedValueRef(const DesignRecord &record,
                                const std::string &moduleName,
                                const std::string &instancePath,
                                const std::string &valueName) {
  ValueRef ref;
  ref.designId = record.id;
  ref.moduleName = moduleName;
  ref.instancePath = instancePath;
  ref.valueName = valueName;
  return ref;
}

SignalTraceStop makeObjectStop(SignalTraceStopReason reason,
                               DesignObjectInfo object) {
  SignalTraceStop stop;
  stop.reason = reason;
  stop.object = std::move(object);
  return stop;
}

SignalTraceStop makeValueStop(SignalTraceStopReason reason, ValueRef value) {
  SignalTraceStop stop;
  stop.reason = reason;
  stop.value = std::move(value);
  return stop;
}

std::optional<DesignObjectInfo>
findObjectForValue(const DesignRecord &record, const DesignModel &model,
                   const ModuleInfo &module, const std::string &pathPrefix,
                   const std::string &instancePath,
                   const std::string &valueName) {
  if (const PortInfo *port = findPort(module, valueName))
    return makePortObject(record, pathPrefix, instancePath, module, *port);
  if (const RegisterInfo *reg = findRegisterForValue(module, valueName))
    return makeRegisterObject(record, pathPrefix, instancePath, module, *reg);
  if (const MemoryPortInfo *memoryPort =
          findMemoryPortForValue(module, valueName))
    return makeMemoryPortObject(record, pathPrefix, instancePath, module,
                                *memoryPort);

  size_t dot = valueName.find('.');
  if (dot == std::string::npos)
    return std::nullopt;

  std::string instanceName = valueName.substr(0, dot);
  std::string portName = valueName.substr(dot + 1);
  const InstanceInfo *instance = findInstance(module, instanceName);
  if (!instance)
    return std::nullopt;
  auto targetIt = model.modules.find(instance->targetModule);
  if (targetIt == model.modules.end())
    return std::nullopt;
  if (const PortInfo *port = findPort(targetIt->second, portName)) {
    return makePortObject(record, joinPath(pathPrefix, instanceName),
                          joinPath(instancePath, instanceName),
                          targetIt->second, *port);
  }
  return std::nullopt;
}

bool isTransparentValue(const ValueInfo &value) {
  return value.kind == ValueKind::OutputPort || value.kind == ValueKind::Wire ||
         value.definingOp == "module output" || value.definingOp == "hw.wire";
}

void appendUniqueStop(std::vector<SignalTraceStop> *stops,
                      SignalTraceStop stop) {
  auto stopKey = [](const SignalTraceStop &value) {
    if (value.object)
      return std::string("o:") + value.object->path + ":" +
             toString(value.reason);
    if (value.value)
      return std::string("v:") + value.value->moduleName + ":" +
             value.value->instancePath + ":" + value.value->valueName + ":" +
             toString(value.reason);
    return std::string("u:") + toString(value.reason);
  };
  std::string key = stopKey(stop);
  auto it = std::find_if(stops->begin(), stops->end(),
                         [&](const SignalTraceStop &existing) {
                           return stopKey(existing) == key;
                         });
  if (it == stops->end())
    stops->push_back(std::move(stop));
}

void traceValueFanin(const DesignRecord &record, const DesignModel &model,
                     const ModuleInfo &module, const std::string &pathPrefix,
                     const std::string &instancePath,
                     const std::string &valueName,
                     std::set<std::string> *active,
                     std::vector<SignalTraceStop> *stops) {
  std::string key = module.name + "|" + instancePath + "|" + valueName;
  if (!active->insert(key).second) {
    appendUniqueStop(
        stops, makeValueStop(SignalTraceStopReason::Cycle,
                             makeUnresolvedValueRef(record, module.name,
                                                    instancePath, valueName)));
    return;
  }

  auto valueIt = module.values.find(valueName);
  if (valueIt == module.values.end()) {
    appendUniqueStop(
        stops, makeValueStop(SignalTraceStopReason::Unresolved,
                             makeUnresolvedValueRef(record, module.name,
                                                    instancePath, valueName)));
    active->erase(key);
    return;
  }

  const ValueInfo &value = valueIt->second;
  if (value.kind == ValueKind::InputPort) {
    if (auto object = findObjectForValue(record, model, module, pathPrefix,
                                         instancePath, valueName)) {
      appendUniqueStop(stops,
                       makeObjectStop(SignalTraceStopReason::Port, *object));
    } else {
      appendUniqueStop(stops, makeValueStop(SignalTraceStopReason::Unresolved,
                                            makeValueRef(record, module,
                                                         instancePath, value)));
    }
    active->erase(key);
    return;
  }

  if (value.kind == ValueKind::Register) {
    if (auto object = findObjectForValue(record, model, module, pathPrefix,
                                         instancePath, valueName)) {
      appendUniqueStop(
          stops, makeObjectStop(SignalTraceStopReason::Register, *object));
    } else {
      appendUniqueStop(stops, makeValueStop(SignalTraceStopReason::Register,
                                            makeValueRef(record, module,
                                                         instancePath, value)));
    }
    active->erase(key);
    return;
  }

  if (value.kind == ValueKind::InstanceResult) {
    if (auto object = findObjectForValue(record, model, module, pathPrefix,
                                         instancePath, valueName)) {
      appendUniqueStop(
          stops,
          makeObjectStop(SignalTraceStopReason::InstanceBoundary, *object));
    } else {
      appendUniqueStop(
          stops,
          makeValueStop(SignalTraceStopReason::InstanceBoundary,
                        makeValueRef(record, module, instancePath, value)));
    }
    active->erase(key);
    return;
  }

  if (findMemoryPortForValue(module, valueName)) {
    if (auto object = findObjectForValue(record, model, module, pathPrefix,
                                         instancePath, valueName)) {
      appendUniqueStop(
          stops, makeObjectStop(SignalTraceStopReason::MemoryPort, *object));
    } else {
      appendUniqueStop(stops, makeValueStop(SignalTraceStopReason::MemoryPort,
                                            makeValueRef(record, module,
                                                         instancePath, value)));
    }
    active->erase(key);
    return;
  }

  if (!isTransparentValue(value)) {
    appendUniqueStop(
        stops,
        makeValueStop(SignalTraceStopReason::CombinationalLogic,
                      makeValueRef(record, module, instancePath, value)));
    active->erase(key);
    return;
  }

  if (value.drivers.empty()) {
    appendUniqueStop(stops, makeValueStop(SignalTraceStopReason::Unresolved,
                                          makeValueRef(record, module,
                                                       instancePath, value)));
    active->erase(key);
    return;
  }

  for (const auto &driver : value.drivers)
    traceValueFanin(record, model, module, pathPrefix, instancePath, driver,
                    active, stops);

  active->erase(key);
}

} // namespace

const QueryService::LoadedDesign *
QueryService::findDesign(const std::string &designId) const {
  auto it = designs.find(designId);
  return it == designs.end() ? nullptr : &it->second;
}

QueryService::LoadedDesign *
QueryService::findDesign(const std::string &designId) {
  auto it = designs.find(designId);
  return it == designs.end() ? nullptr : &it->second;
}

Result<DesignRecord> QueryService::loadDesign(const LoadDesignSpec &spec) {
  if (spec.path.empty())
    return Result<DesignRecord>::failure("design path must be non-empty");

  auto model = parseDesignFile(spec.path, /*includeValueIndex=*/true);
  if (!model.ok)
    return Result<DesignRecord>::failure(model.message);

  IrStatus status = IrStatus::Unknown;
  if (spec.status)
    status = *spec.status;
  else {
    auto inferred = inferDesignFileStatus(spec.path);
    if (!inferred.ok)
      return Result<DesignRecord>::failure(inferred.message);
    status = inferred.value;
  }

  DesignRecord record;
  record.id = "design-" + std::to_string(nextDesignId++);
  record.sourcePath = spec.path;
  record.irStatus = status;
  if (model.value.moduleOrder.size() == 1)
    record.selectedTop = model.value.moduleOrder.front();

  LoadedDesign loaded;
  loaded.record = record;
  loaded.model = std::move(model.value);
  designs[record.id] = std::move(loaded);
  return Result<DesignRecord>::success(record);
}

Result<std::vector<DesignRecord>>
QueryService::loadDesigns(const std::vector<LoadDesignSpec> &specs) {
  std::vector<DesignRecord> records;
  records.reserve(specs.size());
  for (const auto &spec : specs) {
    auto loaded = loadDesign(spec);
    if (!loaded.ok)
      return Result<std::vector<DesignRecord>>::failure(loaded.message);
    records.push_back(std::move(loaded.value));
  }
  return Result<std::vector<DesignRecord>>::success(std::move(records));
}

std::vector<DesignRecord> QueryService::listDesigns() const {
  std::vector<DesignRecord> records;
  for (const auto &[id, loaded] : designs)
    records.push_back(loaded.record);
  return records;
}

Status QueryService::setTop(const std::string &designId,
                            const std::string &moduleName) {
  LoadedDesign *loaded = findDesign(designId);
  if (!loaded)
    return Status::failure("unknown design '" + designId + "'");
  if (loaded->model.modules.find(moduleName) == loaded->model.modules.end())
    return Status::failure("unknown module '" + moduleName + "'");
  loaded->record.selectedTop = moduleName;
  return Status::success();
}

Result<std::vector<ModuleSummary>>
QueryService::listModules(const std::string &designId) const {
  const LoadedDesign *loaded = findDesign(designId);
  if (!loaded)
    return Result<std::vector<ModuleSummary>>::failure("unknown design '" +
                                                       designId + "'");
  std::vector<ModuleSummary> summaries;
  for (const auto &moduleName : loaded->model.moduleOrder) {
    const ModuleInfo &module = loaded->model.modules.at(moduleName);
    summaries.push_back(ModuleSummary{
        module.name, module.ports.size(), module.instances.size(),
        module.operationCount, module.registerCount, module.isExternal});
  }
  return Result<std::vector<ModuleSummary>>::success(std::move(summaries));
}

Result<ModuleInfo>
QueryService::getModule(const std::string &designId,
                        const std::string &moduleName) const {
  const LoadedDesign *loaded = findDesign(designId);
  if (!loaded)
    return Result<ModuleInfo>::failure("unknown design '" + designId + "'");
  auto it = loaded->model.modules.find(moduleName);
  if (it == loaded->model.modules.end())
    return Result<ModuleInfo>::failure("unknown module '" + moduleName + "'");
  return Result<ModuleInfo>::success(it->second);
}

Result<std::vector<PortInfo>>
QueryService::getModulePorts(const std::string &designId,
                             const std::string &moduleName) const {
  auto module = getModule(designId, moduleName);
  if (!module.ok)
    return Result<std::vector<PortInfo>>::failure(module.message);
  return Result<std::vector<PortInfo>>::success(std::move(module.value.ports));
}

Result<std::vector<InstanceInfo>>
QueryService::getModuleInstances(const std::string &designId,
                                 const std::string &moduleName) const {
  auto module = getModule(designId, moduleName);
  if (!module.ok)
    return Result<std::vector<InstanceInfo>>::failure(module.message);
  return Result<std::vector<InstanceInfo>>::success(
      std::move(module.value.instances));
}

Result<std::vector<DesignObjectInfo>>
QueryService::listObjects(const std::string &designId, const std::string &query,
                          MatchMode matchMode,
                          std::optional<ObjectKind> kind) const {
  const LoadedDesign *loaded = findDesign(designId);
  if (!loaded)
    return Result<std::vector<DesignObjectInfo>>::failure("unknown design '" +
                                                          designId + "'");

  std::vector<DesignObjectInfo> objects;
  if (!loaded->record.selectedTop.empty()) {
    std::set<std::string> active;
    collectHierarchyObjects(
        loaded->record, loaded->model, loaded->record.selectedTop,
        loaded->record.selectedTop, "", &active, &objects, kind);
  } else {
    for (const auto &moduleName : loaded->model.moduleOrder) {
      const ModuleInfo &module = loaded->model.modules.at(moduleName);
      collectModuleLocalObjects(loaded->record, module, module.name, "",
                                &objects, kind);
    }
  }

  if (!query.empty()) {
    std::vector<DesignObjectInfo> filtered;
    for (auto &object : objects) {
      if (objectMatches(object, query, matchMode))
        filtered.push_back(std::move(object));
    }
    objects = std::move(filtered);
  }

  std::sort(objects.begin(), objects.end(),
            [](const DesignObjectInfo &lhs, const DesignObjectInfo &rhs) {
              if (lhs.path != rhs.path)
                return lhs.path < rhs.path;
              return static_cast<int>(lhs.kind) < static_cast<int>(rhs.kind);
            });
  return Result<std::vector<DesignObjectInfo>>::success(std::move(objects));
}

Result<NameSearchResult> QueryService::searchNames(const std::string &designId,
                                                   const std::string &query,
                                                   MatchMode matchMode,
                                                   size_t limit) const {
  const LoadedDesign *loaded = findDesign(designId);
  if (!loaded)
    return Result<NameSearchResult>::failure("unknown design '" + designId +
                                             "'");

  NameSearchResult result;
  result.designId = designId;
  result.query = query;
  result.matchMode = matchMode;

  auto addCandidate = [&](NameMatch match, std::vector<std::string> terms) {
    auto score = matchScore(terms, query, matchMode);
    if (!score)
      return;
    match.score = *score;
    result.matches.push_back(std::move(match));
  };

  addCandidate(NameMatch{loaded->record.id, loaded->record.id,
                         EntityKind::Design, "", "", 0},
               {loaded->record.id, loaded->record.sourcePath});

  for (const auto &moduleName : loaded->model.moduleOrder) {
    const ModuleInfo &module = loaded->model.modules.at(moduleName);
    addCandidate(NameMatch{module.name, module.name, EntityKind::Module,
                           module.name, "", 0},
                 {module.name});
    for (const auto &port : module.ports) {
      std::string path = module.name + "/" + port.name;
      addCandidate(
          NameMatch{port.name, path, EntityKind::Port, module.name, "", 0},
          {port.name, path, module.name});
    }
    for (const auto &reg : module.registers) {
      std::string path = module.name + "/" + reg.name;
      addCandidate(
          NameMatch{reg.name, path, EntityKind::Register, module.name, "", 0},
          {reg.name, path, module.name});
    }
    for (const auto &memoryPort : module.memoryPorts) {
      std::string path = module.name + "/" + memoryPort.name;
      addCandidate(NameMatch{memoryPort.name, path, EntityKind::MemoryPort,
                             module.name, "", 0},
                   {memoryPort.name, path, module.name});
    }
    for (const auto &instance : module.instances) {
      std::string path = module.name + "/" + instance.name;
      addCandidate(NameMatch{instance.name, path, EntityKind::Instance,
                             module.name, instance.targetModule, 0},
                   {instance.name, path, module.name, instance.targetModule});
    }
  }

  std::sort(result.matches.begin(), result.matches.end(),
            [](const NameMatch &lhs, const NameMatch &rhs) {
              if (lhs.score != rhs.score)
                return lhs.score < rhs.score;
              if (lhs.path != rhs.path)
                return lhs.path < rhs.path;
              return static_cast<int>(lhs.kind) < static_cast<int>(rhs.kind);
            });
  if (limit != 0 && result.matches.size() > limit)
    result.matches.resize(limit);

  return Result<NameSearchResult>::success(std::move(result));
}

Result<SignalTraceResult> QueryService::traceSignal(const std::string &designId,
                                                    const std::string &toQuery,
                                                    MatchMode matchMode) const {
  const LoadedDesign *loaded = findDesign(designId);
  if (!loaded)
    return Result<SignalTraceResult>::failure("unknown design '" + designId +
                                              "'");
  if (loaded->record.selectedTop.empty()) {
    return Result<SignalTraceResult>::failure(
        "signal trace requires a selected top module; rerun with --top or call "
        "circt_design_top_set first");
  }

  auto objects = listObjects(designId, toQuery, matchMode);
  if (!objects.ok)
    return Result<SignalTraceResult>::failure(objects.message);
  if (objects.value.empty())
    return Result<SignalTraceResult>::failure("signal trace query '" + toQuery +
                                              "' matched no objects");
  if (objects.value.size() != 1) {
    return Result<SignalTraceResult>::failure(
        "signal trace query '" + toQuery +
        "' must resolve to exactly one object, matched " +
        std::to_string(objects.value.size()));
  }

  SignalTraceResult result;
  result.designId = designId;
  result.topModuleName = loaded->record.selectedTop;
  result.toQuery = toQuery;
  result.matchMode = matchMode;
  result.targetObject = std::move(objects.value.front());

  auto moduleIt = loaded->model.modules.find(result.targetObject.moduleName);
  if (moduleIt == loaded->model.modules.end())
    return Result<SignalTraceResult>::failure(
        "target object refers to unknown module '" +
        result.targetObject.moduleName + "'");

  const ModuleInfo &module = moduleIt->second;
  std::string pathPrefix = getObjectParentPath(result.targetObject);
  std::set<std::string> active;

  switch (result.targetObject.kind) {
  case ObjectKind::Port:
    if (result.targetObject.direction == PortDirection::Input) {
      appendUniqueStop(
          &result.stops,
          makeObjectStop(SignalTraceStopReason::Port, result.targetObject));
      break;
    }
    traceValueFanin(loaded->record, loaded->model, module, pathPrefix,
                    result.targetObject.instancePath,
                    result.targetObject.localName, &active, &result.stops);
    break;
  case ObjectKind::Register:
    appendUniqueStop(
        &result.stops,
        makeObjectStop(SignalTraceStopReason::Register, result.targetObject));
    result.diagnostics.push_back(
        "register objects are treated as sequential trace boundaries");
    break;
  case ObjectKind::MemoryPort:
    appendUniqueStop(
        &result.stops,
        makeObjectStop(SignalTraceStopReason::MemoryPort, result.targetObject));
    result.diagnostics.push_back(
        "memory port objects are treated as trace boundaries");
    break;
  case ObjectKind::Instance:
    appendUniqueStop(&result.stops,
                     makeObjectStop(SignalTraceStopReason::InstanceBoundary,
                                    result.targetObject));
    result.diagnostics.push_back(
        "instance objects are treated as hierarchy trace boundaries");
    break;
  }

  return Result<SignalTraceResult>::success(std::move(result));
}

} // namespace circt_query
