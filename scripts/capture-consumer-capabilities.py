#!/usr/bin/env python3
"""Build/run the direct-ICD G615 consumer probe and validate its JSON."""
import argparse, datetime, hashlib, json, os, pathlib, subprocess, sys, tempfile, xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXPECTED_DEVICE = 0xB8A31030
EXPECTED_SHA = "576e37de9a3b60dda9a972a6f91c3a50e09238bd31a9888e04215c7b554c803a"
REQUIRED_FEATURES = {"robustBufferAccess2", "nullDescriptor", "descriptorIndexing", "dynamicRendering", "geometryShader", "textureCompressionBC", "sparseBinding"}
REQUIRED_PROPERTIES = {"maxPushConstantsSize", "maxPushDescriptors", "maxCustomBorderColorSamplers", "bufferImageGranularity", "maxUpdateAfterBindDescriptorsInAllPools"}
REGISTRY = ROOT / "work/mesa/src/vulkan/registry/vk.xml"

def registry_inventory(extensions):
    root = ET.parse(REGISTRY).getroot()
    extension_nodes = [ext for ext in root.findall("./extensions/extension")
                       if "vulkan" in (ext.get("supported") or "").split(",")]
    core_nodes = [feature for feature in root.findall("./feature")
                  if "vulkan" in (feature.get("api") or "vulkan").split(",")]
    inventory = {"featureStructures": {}, "propertyStructures": {}}
    for base, section in (("VkPhysicalDeviceFeatures2", "featureStructures"), ("VkPhysicalDeviceProperties2", "propertyStructures")):
        for struct in root.findall("./types/type[@category='struct']"):
            name = struct.get("name") or struct.findtext("name") or ""
            if base not in (struct.get("structextends") or "").split(","):
                continue
            owners = [ext.get("name") for ext in extension_nodes
                      if ext.get("name") in extensions and ext.find(f".//type[@name='{name}']") is not None]
            core = [feature.get("number") for feature in core_nodes
                    if feature.find(f".//type[@name='{name}']") is not None]
            if not owners and not core:
                continue
            fields = [member.findtext("name") for member in struct.findall("member")
                      if member.findtext("name") not in ("sType", "pNext")]
            inventory[section][name] = {"fields": fields, "extensions": owners, "core": core}
    return inventory

def struct_fields(root, name):
    struct = next(node for node in root.findall("./types/type[@category='struct']")
                  if (node.get("name") or node.findtext("name")) == name)
    result = {}
    struct_names = {node.get("name") or node.findtext("name") for node in root.findall("./types/type[@category='struct']")}
    for member in struct.findall("member"):
        field, field_type = member.findtext("name"), member.findtext("type")
        if field in ("sType", "pNext"):
            continue
        result[field] = struct_fields(root, field_type) if field_type in struct_names and not member.get("len") else None
    return result

def validate_fields(actual, expected, name):
    if set(actual) != set(expected): raise ValueError(f"{name} field coverage mismatch")
    for field, nested in expected.items():
        if nested is not None:
            if not isinstance(actual[field], dict): raise ValueError(f"{name}.{field} must be an object")
            validate_fields(actual[field], nested, f"{name}.{field}")

def generated_probe_source(template, extensions):
    root = ET.parse(REGISTRY).getroot()
    declarations, feature_json, property_json = [], [], []
    inventory = registry_inventory(extensions)
    for section, base, call, output in (
        ("featureStructures", "VkPhysicalDeviceFeatures2", "get_features", feature_json),
        ("propertyStructures", "VkPhysicalDeviceProperties2", "get_properties", property_json),
    ):
        head = "generated_features" if section == "featureStructures" else "generated_properties"
        seen = {}
        declarations.append(f" void *{head} = NULL;")
        records = []
        for index, (name, meta) in enumerate(inventory[section].items()):
            struct = next(x for x in root.findall("./types/type[@category='struct']") if (x.get("name") or x.findtext("name")) == name)
            stype_member = next((x for x in struct.findall("member") if x.findtext("name") == "sType"), None)
            if stype_member is None or not stype_member.get("values"):
                continue
            stype = stype_member.get("values").split(",")[0]
            condition = " || ".join([f"p2.properties.apiVersion >= VK_MAKE_API_VERSION(0,{v.split('.')[0]},{v.split('.')[1]},0)" for v in meta["core"]] +
                                    [f'has_extension(exts,ext_count,"{ext}")' for ext in meta["extensions"]]) or "0"
            if stype in seen:
                variable = seen[stype]
            else:
                variable = f"generated_{section}_{index}"
                seen[stype] = variable
                declarations.append(f" {name} {variable} = {{{stype}}}; if ({condition}) CHAIN({head},{variable});")
            records.append((name, variable, struct, condition))
        root_name = f"generated_{section}_root"
        declarations.append(f" {base} {root_name} = {{VK_STRUCTURE_TYPE_{'PHYSICAL_DEVICE_FEATURES_2' if section == 'featureStructures' else 'PHYSICAL_DEVICE_PROPERTIES_2'}, {head}}}; {call}(physical,&{root_name});")
        pointer_fields = []
        if section == "propertyStructures":
            allocated = set()
            for _, variable, struct, _ in records:
                for member in struct.findall("member"):
                    raw, field, length = "".join(member.itertext()), member.findtext("name"), member.get("len")
                    key = f"{variable}.{field}"
                    if "*" not in raw or field == "pNext" or not length or key in allocated:
                        continue
                    declarations.append(f" if ({variable}.{length}) {variable}.{field}=calloc({variable}.{length},sizeof(*{variable}.{field}));")
                    pointer_fields.append(f"{variable}.{field}")
                    allocated.add(key)
            if pointer_fields:
                declarations.append(f" {call}(physical,&{root_name});")
        lines = [f' printf(",\\\"{section}\\\":{{"); first=1;']
        for name, variable, struct, condition in records:
            lines.append(f' if ({condition}) {{ if(!first)putchar(\',\');first=0;json_string("{name}");printf(":{{");int field_first=1;')
            for member in struct.findall("member"):
                field = member.findtext("name")
                if field in ("sType", "pNext"):
                    continue
                raw = "".join(member.itertext())
                field_type = member.findtext("type")
                if "*" in raw and member.get("len"):
                    value = f'json_bytes({variable}.{field},sizeof(*{variable}.{field})*{variable}.{member.get("len")})'
                elif field_type == "VkBool32" and not member.get("len"):
                    value = f'printf("%s",{variable}.{field}?"true":"false")'
                elif not member.get("len") and field_type in ("float", "double"):
                    value = f'printf("%.17g",(double){variable}.{field})'
                elif not member.get("len") and field_type in ("int8_t", "int16_t", "int32_t", "int64_t"):
                    value = f'printf("%" PRId64,(int64_t){variable}.{field})'
                elif not member.get("len") and field_type not in {"char", "VkConformanceVersion", "VkPhysicalDeviceLayeredApiPropertiesKHR", "VkDefaultVertexAttributeValueKHR"}:
                    value = f'printf("%" PRIu64,(uint64_t){variable}.{field})'
                else:
                    value = f'json_bytes(&{variable}.{field},sizeof({variable}.{field}))'
                lines.append(f' if(!field_first)putchar(\',\');field_first=0;json_string("{field}");putchar(\':\');{value};')
            lines.append(' printf("}"); }')
        lines.append(' printf("}");')
        if section == "propertyStructures":
            lines.extend(f" free({field});" for field in pointer_fields)
        output.extend(lines)
    declarations_text = "\n".join(declarations)
    def emit_struct(name, expression):
        struct = next(node for node in root.findall("./types/type[@category='struct']")
                      if (node.get("name") or node.findtext("name")) == name)
        struct_names = {node.get("name") or node.findtext("name") for node in root.findall("./types/type[@category='struct']")}
        lines = ['do { putchar(\'{\');int nested_first=1;']
        for member in struct.findall("member"):
            field, field_type = member.findtext("name"), member.findtext("type")
            if field in ("sType", "pNext"):
                continue
            lines.append(f'if(!nested_first)putchar(\',\');nested_first=0;json_string("{field}");putchar(\':\');')
            if field_type in struct_names and not member.get("len"):
                lines.extend(emit_struct(field_type, f"{expression}.{field}"))
            elif field_type == "VkBool32" and not member.get("len"):
                lines.append(f'printf("%s",{expression}.{field}?"true":"false");')
            else:
                lines.append(f'json_bytes(&{expression}.{field},sizeof({expression}.{field}));')
        lines.append("putchar('}'); } while(0);")
        return lines
    core_json = ['printf(",\\\"coreProperties\\\":");'] + emit_struct("VkPhysicalDeviceProperties", "p2.properties")
    json_text = "\n".join(core_json + feature_json + property_json)
    return template.replace("/* CAPTURE_GENERATED_DECLARATIONS */", declarations_text).replace("/* CAPTURE_GENERATED_JSON */", json_text)

def validate(data):
    if data.get("schemaVersion") != 1: raise ValueError("unsupported schemaVersion")
    if data.get("device", {}).get("deviceID") != EXPECTED_DEVICE: raise ValueError("not Mali-G615 0xb8a31030")
    if not REQUIRED_FEATURES <= data.get("features", {}).keys(): raise ValueError("required features missing")
    if not REQUIRED_PROPERTIES <= data.get("properties", {}).keys(): raise ValueError("required properties missing")
    if len(data.get("bcFormats", [])) != 16: raise ValueError("all 16 BC formats required")
    if len({x.get("name") for x in data["extensions"]}) != len(data["extensions"]): raise ValueError("duplicate extensions")
    expected = registry_inventory({x["name"] for x in data["extensions"]})
    root = ET.parse(REGISTRY).getroot()
    core_features = set(struct_fields(root, "VkPhysicalDeviceFeatures"))
    if not core_features <= set(data.get("features", {})): raise ValueError("VkPhysicalDeviceFeatures field coverage mismatch")
    validate_fields(data.get("coreProperties", {}), struct_fields(root, "VkPhysicalDeviceProperties"), "VkPhysicalDeviceProperties")
    for section in expected:
        actual = data.get(section, {})
        if set(actual) != set(expected[section]): raise ValueError(f"{section} structure coverage mismatch")
        for name, meta in expected[section].items():
            if set(actual[name]) != set(meta["fields"]): raise ValueError(f"{name} field coverage mismatch")
    expected_bc = {"VK_FORMAT_BC1_RGB_UNORM_BLOCK", "VK_FORMAT_BC1_RGB_SRGB_BLOCK", "VK_FORMAT_BC1_RGBA_UNORM_BLOCK", "VK_FORMAT_BC1_RGBA_SRGB_BLOCK", "VK_FORMAT_BC2_UNORM_BLOCK", "VK_FORMAT_BC2_SRGB_BLOCK", "VK_FORMAT_BC3_UNORM_BLOCK", "VK_FORMAT_BC3_SRGB_BLOCK", "VK_FORMAT_BC4_UNORM_BLOCK", "VK_FORMAT_BC4_SNORM_BLOCK", "VK_FORMAT_BC5_UNORM_BLOCK", "VK_FORMAT_BC5_SNORM_BLOCK", "VK_FORMAT_BC6H_UFLOAT_BLOCK", "VK_FORMAT_BC6H_SFLOAT_BLOCK", "VK_FORMAT_BC7_UNORM_BLOCK", "VK_FORMAT_BC7_SRGB_BLOCK"}
    if {item.get("name") for item in data["bcFormats"]} != expected_bc: raise ValueError("exact 16 BC formats required")
    for item in data["bcFormats"]:
        if set(item) != {"name", "legacy", "properties2", "properties3", "imageFormatResult", "imageFormatProperties"}: raise ValueError("BC query coverage mismatch")
        if any(set(item[key]) != {"linear", "optimal", "buffer"} for key in ("legacy", "properties2", "properties3")): raise ValueError("BC format property coverage mismatch")
        if set(item["imageFormatProperties"]) != {"maxExtent", "maxMipLevels", "maxArrayLayers", "sampleCounts", "maxResourceSize"}: raise ValueError("BC image format property coverage mismatch")
        if set(item["imageFormatProperties"]["maxExtent"]) != {"width", "height", "depth"}: raise ValueError("BC maxExtent coverage mismatch")
    return data

def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", default="validation/g615-v11-csf/consumer-capabilities.json")
    ap.add_argument("--raw-output", help="optional separate byte-for-byte raw probe output")
    ap.add_argument("--input", help="validate existing raw probe JSON instead of using adb")
    ap.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL", "Y5WWBMJVOZSK4HU8"))
    ap.add_argument("--icd", default="/data/local/tmp/libvulkan_panfrost.so")
    a = ap.parse_args()
    if a.input:
        raw = pathlib.Path(a.input).read_text()
    else:
        ndk = pathlib.Path(os.environ.get("ANDROID_NDK_ROOT", "/opt/android-sdk/ndk/30.0.14904198"))
        cc = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang"
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / "g615-consumer-caps"
            source = pathlib.Path(temp) / "g615_consumer_caps.c"
            template = (ROOT / "tests/consumer-compat/g615_consumer_caps.c").read_text()
            existing = json.loads((ROOT / "validation/g615-v11-csf/consumer-capabilities.json").read_text())
            source.write_text(generated_probe_source(template, {x["name"] for x in existing["extensions"]}))
            subprocess.run([str(cc), "-std=c11", "-O2", "-fPIE", "-pie", "-I", str(ROOT / "work/mesa/include"), str(source), "-ldl", "-o", str(binary)], check=True)
            remote_sha = subprocess.run(["adb", "-s", a.serial, "shell", "sha256sum", a.icd], check=True, text=True, capture_output=True).stdout.split()[0]
            if remote_sha != EXPECTED_SHA: raise SystemExit(f"ICD SHA mismatch: {remote_sha}")
            subprocess.run(["adb", "-s", a.serial, "push", str(binary), "/data/local/tmp/g615-consumer-caps"], check=True, capture_output=True)
            subprocess.run(["adb", "-s", a.serial, "shell", "chmod", "755", "/data/local/tmp/g615-consumer-caps"], check=True)
            raw = subprocess.run(["adb", "-s", a.serial, "shell", "/data/local/tmp/g615-consumer-caps", a.icd], check=True, text=True, capture_output=True, timeout=30).stdout
    data = validate(json.loads(raw))
    data["capture"] = {"timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(), "command": "direct vk_icdGetInstanceProcAddr probe", "status": "PASS", "icdSha256": EXPECTED_SHA, "target": "Poco X6 Pro / duchamp"}
    output = ROOT / a.output
    output.parent.mkdir(parents=True, exist_ok=True)
    if a.raw_output:
        raw_output = ROOT / a.raw_output; raw_output.parent.mkdir(parents=True, exist_ok=True)
        raw_output.write_text(raw if raw.endswith("\n") else raw + "\n")
    output.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    print(f"PASS device=0x{EXPECTED_DEVICE:x} extensions={len(data['extensions'])} bc=16 output={output}")
    return 0

if __name__ == "__main__": sys.exit(main())
