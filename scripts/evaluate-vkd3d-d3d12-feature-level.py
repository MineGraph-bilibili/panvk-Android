#!/usr/bin/env python3
"""P21: D3D12 feature/profile analysis for vkd3d-proton 2.0, 2.14.1, 3.0.1.

Does not flip features. Does not implement missing capabilities. Does not
start sparse/Kbase work. Keeps HARD_REQUIREMENTS, PROFILE_BASELINE,
DEVICE_CREATE, and FEATURE_LEVEL separate.
"""
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CAPS_PATH = ROOT / "validation/g615-v11-csf/consumer-capabilities.json"
REQ_2_0 = ROOT / "validation/requirements/vkd3d-proton-2.0.json"
P20_EXTRAS = ROOT / "validation/g615-v11-csf/p20-remaining-baseline-extras.json"
JSON_OUT = ROOT / "validation/g615-v11-csf/p21-d3d12-feature-level.json"
MD_OUT = ROOT / "validation/g615-v11-csf/P21-D3D12-FEATURE-LEVEL.md"
TXT_OUT = ROOT / "validation/g615-v11-csf/p21-d3d12-feature-level-2026-09-20.txt"
VKD3D_REPO = "https://github.com/HansKristian-Work/vkd3d-proton"
BASELINE_PROFILE = "VP_D3D12_FL_11_0_baseline"
FEATURE_LEVEL_PROFILES = (
    "VP_D3D12_FL_11_0_baseline",
    "VP_D3D12_FL_11_1_baseline",
    "VP_D3D12_FL_12_0_baseline",
    "VP_D3D12_FL_12_1_baseline",
    "VP_D3D12_FL_12_2_baseline",
)
VENDOR_PROFILES = ("VP_D3D12_maximum_radv", "VP_D3D12_maximum_nv")
D3D12_UAV_SLOT_COUNT = 64
FALSE_BITS = (
    "geometryShader",
    "fillModeNonSolid",
    "shaderClipDistance",
    "shaderCullDistance",
    "textureCompressionBC",
    "multiViewport",
    "transformFeedback",
    "geometryStreams",
    "tessellationShader",
    "pipelineStatisticsQuery",
    "robustImageAccess2",
    "sparseBinding",
    "sparseResidencyBuffer",
    "sparseResidencyImage2D",
    "sparseResidencyAliased",
    "shaderResourceResidency",
    "shaderResourceMinLod",
    "vertexPipelineStoresAndAtomics",
)

P21_CLASSIFICATION = {
    "pipelineStatisticsQuery": {
        "upstreamPanVK": "ABSENT at pinned 5a07217 and origin/main e1f3f372; P20 BLOCKED_SAFE_FALSE; CreateQueryPool UNREACHABLE for PIPELINE_STATISTICS",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Complete pipeline-statistics query path, then CTS. Never expose early.",
    },
    "sparseBinding": {
        "upstreamPanVK": "Pinned G615 reports false. Upstream PanVK sparse is pan-kmod/Panthor, not this Kbase backend. P22 not started.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. P22 Kbase sparse feasibility first. Do not jump to sparse implementation.",
    },
    "sparseResidencyBuffer": {
        "upstreamPanVK": "Pinned G615 reports false. Depends on sparseBinding plus Kbase VM map/unmap. P22 not started.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. P22 Kbase sparse feasibility first. Do not jump to sparse implementation.",
    },
    "sparseResidencyImage2D": {
        "upstreamPanVK": "Pinned G615 reports false. Depends on sparseBinding plus image sparse bind. P22 not started.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. P22 Kbase sparse feasibility first. Do not jump to sparse implementation.",
    },
    "sparseResidencyAliased": {
        "upstreamPanVK": "Pinned G615 reports false. Depends on sparse alias mappings. P22 not started.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. P22 Kbase sparse feasibility first. Do not jump to sparse implementation.",
    },
    "shaderResourceResidency": {
        "upstreamPanVK": "Pinned G615 reports false. Needed for D3D12 tiled-resource residency queries. P22 not started.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. P22 Kbase sparse feasibility first. Do not jump to sparse implementation.",
    },
    "shaderResourceMinLod": {
        "upstreamPanVK": "Pinned G615 reports false. Needed for D3D12 tiled-resource min LOD. P22 not started.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. P22 Kbase sparse feasibility first. Do not jump to sparse implementation.",
    },
    "VK_QUEUE_SPARSE_BINDING_BIT": {
        "upstreamPanVK": "Capture has no sparse-binding queue family. P22 not started.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep absent. P22 Kbase sparse feasibility first. Do not jump to sparse implementation.",
    },
    "residencyStandard2DBlockShape": {
        "upstreamPanVK": "G615 sparseProperties.residencyStandard2DBlockShape is true, but sparseBinding is false so tiled resources stay unsupported.",
        "implementationStatus": "NOT_SUFFICIENT",
        "action": "No driver change in P21. Sparse features remain false.",
    },
    "residencyNonResidentStrict": {
        "upstreamPanVK": "G615 reports false. vkd3d tiled tier 2 requires it together with shaderResourceResidency/MinLod.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. P22 Kbase sparse feasibility first.",
    },
    "residencyAlignedMipSize": {
        "upstreamPanVK": "G615 reports false. vkd3d tiled tier 2 requires residencyAlignedMipSize to stay false.",
        "implementationStatus": "IMPLEMENTED",
        "action": "None. This property already matches the vkd3d tiled-tier-2 polarity.",
    },
    "vertexPipelineStoresAndAtomics": {
        "upstreamPanVK": "Pinned G615 reports false. vkd3d d3d12_device_caps_init_feature_level needs it for D3D_FEATURE_LEVEL_11_1.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Do not expose until VS/GS UAV stores and atomics are proven. Blocks implied FL 11_1.",
    },
    "logicOp": {
        "upstreamPanVK": "Present in pinned G615 capture",
        "implementationStatus": "IMPLEMENTED",
        "action": "None",
    },
    "VK_EXT_fragment_shader_interlock": {
        "upstreamPanVK": "Not advertised. vkd3d ROVsSupported needs fragmentShaderPixelInterlock and fragmentShaderSampleInterlock.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep absent. Needed only for implied FL 12_1 ROVs after FL 12_0.",
    },
    "fragmentShaderPixelInterlock": {
        "upstreamPanVK": "Not advertised. vkd3d ROVsSupported is false without this bit.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Needed only for implied FL 12_1 after FL 12_0.",
    },
    "fragmentShaderSampleInterlock": {
        "upstreamPanVK": "Not advertised. vkd3d ROVsSupported is false without this bit.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Needed only for implied FL 12_1 after FL 12_0.",
    },
    "VK_EXT_mesh_shader": {
        "upstreamPanVK": "Not advertised. vkd3d FL 12_2 needs MeshShaderTier.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep absent. Higher than first D3D12 device-create gate.",
    },
    "VK_KHR_ray_tracing_pipeline": {
        "upstreamPanVK": "Not advertised. vkd3d FL 12_2 needs RaytracingTier 1.1.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep absent. Higher than first D3D12 device-create gate.",
    },
    "VK_KHR_fragment_shading_rate": {
        "upstreamPanVK": "Not advertised. vkd3d FL 12_2 needs VariableShadingRateTier 2.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep absent. Higher than first D3D12 device-create gate.",
    },
    "shaderOutputViewportIndex": {
        "upstreamPanVK": "Pinned G615 reports false. vkd3d uses it with shaderOutputLayer for VPAndRTArrayIndexFromAnyShaderFeedingRasterizer.",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Needed for implied FL 12_2, not FL 11_0.",
    },
    "transformFeedbackQueries": {
        "upstreamPanVK": "ABSENT at pinned and origin/main; XFB queries need EXT_transform_feedback (P15 BLOCKED_SAFE_FALSE)",
        "implementationStatus": "NOT_IMPLEMENTED",
        "action": "Keep false. Complete XFB queries after transformFeedback, then CTS. Never expose early. Also a 2.14.1/3.0.1 DEVICE_CREATE E_INVALIDARG.",
    },
}


def load_p19():
    spec = importlib.util.spec_from_file_location(
        "evaluate_vkd3d_profile_baseline",
        ROOT / "scripts/evaluate-vkd3d-profile-baseline.py",
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CLASSIFICATION.update(P21_CLASSIFICATION)
    return module


def requirement_row(p19, name, actual, required, source_version, source, status):
    row = p19.requirement_row(name, actual, required, source_version, source, status)
    extra = P21_CLASSIFICATION.get(name)
    if extra and status != "PASS":
        row.update(extra)
    return row


def sparse_queue_present(caps):
    for family in caps.get("queueFamilies", caps.get("queues", [])):
        flags = family.get("queueFlags", family.get("flags", 0))
        if isinstance(flags, str) and "SPARSE" in flags:
            return True
        if isinstance(flags, int) and flags & 0x00000008:
            return True
        if isinstance(flags, list) and any("SPARSE" in str(item) for item in flags):
            return True
    return False


def property_value(caps, name):
    props = caps.get("properties", {})
    if name in props:
        return props[name]
    for struct in caps.get("propertyStructures", {}).values():
        if name in struct:
            return struct[name]
    core = caps.get("coreProperties", {})
    if name in core:
        return core[name]
    limits = core.get("limits", {})
    if name in limits:
        return limits[name]
    sparse = core.get("sparseProperties", {})
    if name in sparse:
        return sparse[name]
    top = caps.get("sparseProperties", {})
    if name in top:
        return top[name]
    return None


def sparse_property(caps, name):
    return property_value(caps, name)


def evaluate_hard_2_0(p19, caps):
    manifest = json.loads(REQ_2_0.read_text())
    hard = manifest["hardRequirements"]
    source = f"v2.0 {hard['source']} README.md hard requirements (Vulkan 1.1, VK_EXT_descriptor_indexing, 1_000_000 UpdateAfterBind except UniformBuffer, VkPhysicalDeviceDescriptorIndexingFeatures, VK_KHR_timeline_semaphore)"
    rows = []
    api_ok = p19.api_tuple(caps) >= (1, 1, 0)
    rows.append(
        requirement_row(
            p19,
            "apiVersion>=1.1",
            p19.api_text(caps),
            "1.1",
            "2.0",
            source,
            "PASS" if api_ok else "FAIL",
        )
    )
    indexing_ext = p19.extension_present(caps, "VK_EXT_descriptor_indexing")
    descriptor_indexing = p19.feature_value(caps, "descriptorIndexing")
    rows.append(
        requirement_row(
            p19,
            "descriptorIndexing",
            descriptor_indexing if descriptor_indexing is not None else indexing_ext,
            True,
            "2.0",
            source,
            "PASS" if descriptor_indexing is True or indexing_ext is not None else "FAIL",
        )
    )
    idx = p19.indexing_features(caps)
    missing = [
        name
        for name, value in idx.items()
        if name not in p19.INDEXING_EXCLUDED and value is not True
    ]
    rows.append(
        requirement_row(
            p19,
            "VkPhysicalDeviceDescriptorIndexingFeatures except UniformBuffer UAB",
            not missing,
            True,
            "2.0",
            source,
            "PASS" if idx and not missing else "FAIL",
        )
    )
    for name in p19.UAB_LIMITS:
        actual = p19.uab_value(caps, name)
        status = "PASS" if actual is not None and actual >= p19.UAB_MINIMUM else "FAIL"
        rows.append(requirement_row(p19, name, actual, p19.UAB_MINIMUM, "2.0", source, status))
    timeline = p19.extension_present(caps, "VK_KHR_timeline_semaphore")
    rows.append(
        requirement_row(
            p19,
            "VK_KHR_timeline_semaphore",
            timeline,
            1,
            "2.0",
            source,
            "PASS" if timeline is not None else "FAIL",
        )
    )
    fails = [row for row in rows if row["status"] == "FAIL"]
    recommended = []
    rec = manifest["recommended"]
    rec_source = f"v2.0 {rec['source']} README.md recommended, not mandatory"
    for extension in rec["extensions"]:
        actual = p19.extension_present(caps, extension)
        recommended.append(
            requirement_row(
                p19,
                extension,
                actual,
                1,
                "2.0",
                rec_source,
                "PASS" if actual is not None else "FAIL",
            )
        )
    return {
        "status": "PASS" if not fails else "FAIL",
        "sourceKind": "readme-hard-requirements",
        "sourceFile": f"{VKD3D_REPO}/blob/v2.0/README.md",
        "source": source,
        "passCount": len(rows) - len(fails),
        "failCount": len(fails),
        "requirements": rows,
        "blockers": [row["requirement"] for row in fails],
        "recommended": recommended,
        "note": (
            "vkd3d-proton 2.0 has no VP_D3D12_VKD3D_PROTON_profile.json. "
            "HARD_REQUIREMENTS come from the tagged README. Recommended extensions "
            "are not this HARD gate. robustImageAccess2 is not a 2.0 DEVICE_CREATE check."
        ),
    }


def evaluate_device_create_2_0(p19, caps):
    source = (
        "v2.0 libs/vkd3d/device.c vkd3d_init_device_caps "
        "(KHR_timeline_semaphore is the only E_INVALIDARG hard gate)"
    )
    timeline = p19.extension_present(caps, "VK_KHR_timeline_semaphore")
    api_ok = p19.api_tuple(caps) >= (1, 1, 0)
    checks = [
        ("apiVersion>=1.1", p19.api_text(caps), "1.1", api_ok),
        ("VK_KHR_timeline_semaphore", timeline, 1, timeline is not None),
    ]
    rows = [
        requirement_row(p19, name, actual, required, "2.0", source, "PASS" if ok else "FAIL")
        for name, actual, required, ok in checks
    ]
    fails = [row for row in rows if row["status"] == "FAIL"]
    smoke_reason = (
        "Stock vkd3d-proton v2.0 is a Windows D3D12-on-Vulkan layer. This Android ICD "
        "is not a legal host for the tagged binaries. No stock vkd3d binary was launched. "
        "vkd3d_init_device_caps would not return E_INVALIDARG on the captured timeline "
        "semaphore bit, but that is not a stock smoke PASS."
    )
    return {
        "status": "PASS" if not fails else "FAIL",
        "sourceKind": "vkd3d-device-create",
        "sourceFile": f"{VKD3D_REPO}/blob/v2.0/libs/vkd3d/device.c",
        "source": source,
        "passCount": len(rows) - len(fails),
        "failCount": len(fails),
        "requirements": rows,
        "blockers": [row["requirement"] for row in fails],
        "stockVkd3dSmoke": "BLOCKED",
        "smokeReason": smoke_reason,
    }


def evaluate_device_create_later(p19, caps, version):
    source = (
        f"v{version} libs/vkd3d/device.c vkd3d_init_device_caps E_INVALIDARG gates "
        "(VK_EXT_vertex_attribute_divisor spec>=3, transformFeedbackQueries, "
        "single texel alignment, samplerMirrorClampToEdge, robustBufferAccess2, "
        "robustImageAccess2, nullDescriptor, shaderDrawParameters, VK_KHR_push_descriptor)"
    )
    divisor_ext = p19.extension_present(caps, "VK_EXT_vertex_attribute_divisor")
    divisor = p19.feature_value(caps, "vertexAttributeInstanceRateDivisor") is True
    zero_divisor = p19.feature_value(caps, "vertexAttributeInstanceRateZeroDivisor") is True
    xfb_queries = property_value(caps, "transformFeedbackQueries") is True
    storage_texel = property_value(caps, "storageTexelBufferOffsetSingleTexelAlignment") is True
    uniform_texel = property_value(caps, "uniformTexelBufferOffsetSingleTexelAlignment") is True
    checks = [
        (
            "apiVersion>=1.3",
            p19.api_text(caps),
            "1.3",
            p19.api_tuple(caps) >= (1, 3, 0),
        ),
        (
            "VK_EXT_vertex_attribute_divisor",
            divisor_ext,
            3,
            divisor_ext is not None and divisor_ext >= 3 and divisor and zero_divisor,
        ),
        (
            "transformFeedbackQueries",
            property_value(caps, "transformFeedbackQueries"),
            True,
            xfb_queries,
        ),
        (
            "storageTexelBufferOffsetSingleTexelAlignment",
            property_value(caps, "storageTexelBufferOffsetSingleTexelAlignment"),
            True,
            storage_texel,
        ),
        (
            "uniformTexelBufferOffsetSingleTexelAlignment",
            property_value(caps, "uniformTexelBufferOffsetSingleTexelAlignment"),
            True,
            uniform_texel,
        ),
        (
            "samplerMirrorClampToEdge",
            p19.feature_value(caps, "samplerMirrorClampToEdge"),
            True,
            p19.feature_value(caps, "samplerMirrorClampToEdge") is True,
        ),
        (
            "shaderDrawParameters",
            p19.feature_value(caps, "shaderDrawParameters"),
            True,
            p19.feature_value(caps, "shaderDrawParameters") is True,
        ),
        (
            "VK_EXT_robustness2",
            p19.extension_present(caps, "VK_EXT_robustness2"),
            1,
            p19.extension_present(caps, "VK_EXT_robustness2") is not None,
        ),
        (
            "robustBufferAccess2",
            p19.feature_value(caps, "robustBufferAccess2"),
            True,
            p19.feature_value(caps, "robustBufferAccess2") is True,
        ),
        (
            "robustImageAccess2",
            p19.feature_value(caps, "robustImageAccess2"),
            True,
            p19.feature_value(caps, "robustImageAccess2") is True,
        ),
        (
            "nullDescriptor",
            p19.feature_value(caps, "nullDescriptor"),
            True,
            p19.feature_value(caps, "nullDescriptor") is True,
        ),
        (
            "VK_KHR_push_descriptor",
            p19.extension_present(caps, "VK_KHR_push_descriptor"),
            1,
            p19.extension_present(caps, "VK_KHR_push_descriptor") is not None,
        ),
    ]
    rows = [
        requirement_row(p19, name, actual, required, version, source, "PASS" if ok else "FAIL")
        for name, actual, required, ok in checks
    ]
    fails = [row for row in rows if row["status"] == "FAIL"]
    smoke_reason = (
        f"Stock vkd3d-proton v{version} vkd3d_init_device_caps returns E_INVALIDARG when "
        "robustImageAccess2 or transformFeedbackQueries is false. G615 reports both false. "
        "Stock vkd3d-proton is a Windows D3D12-on-Vulkan layer; this Android ICD is not a "
        "legal host for the tagged binaries. No stock vkd3d binary was launched."
    )
    return {
        "status": "PASS" if not fails else "FAIL",
        "sourceKind": "vkd3d-device-create",
        "sourceFile": f"{VKD3D_REPO}/blob/v{version}/libs/vkd3d/device.c",
        "source": source,
        "passCount": len(rows) - len(fails),
        "failCount": len(fails),
        "requirements": rows,
        "blockers": [row["requirement"] for row in fails],
        "stockVkd3dSmoke": "BLOCKED",
        "smokeReason": smoke_reason,
    }


def tiled_resources_tier(caps):
    source = (
        "libs/vkd3d/device.c d3d12_device_determine_tiled_resources_tier "
        "(sparseBinding, sparseResidencyAliased, sparseResidencyBuffer, "
        "sparseResidencyImage2D, residencyStandard2DBlockShape, sparse queue)"
    )
    features = {
        "sparseBinding": caps["features"]["sparseBinding"] is True,
        "sparseResidencyAliased": caps["features"]["sparseResidencyAliased"] is True,
        "sparseResidencyBuffer": caps["features"]["sparseResidencyBuffer"] is True,
        "sparseResidencyImage2D": caps["features"]["sparseResidencyImage2D"] is True,
        "residencyStandard2DBlockShape": sparse_property(caps, "residencyStandard2DBlockShape") is True,
        "VK_QUEUE_SPARSE_BINDING_BIT": sparse_queue_present(caps),
    }
    tier1 = all(features.values())
    residency = caps["features"]["shaderResourceResidency"] is True
    min_lod = caps["features"]["shaderResourceMinLod"] is True
    aligned = sparse_property(caps, "residencyAlignedMipSize") is False
    non_resident = sparse_property(caps, "residencyNonResidentStrict") is True
    filter_minmax = caps["features"].get("samplerFilterMinmax") is True or (
        caps.get("propertyStructures", {})
        .get("VkPhysicalDeviceVulkan12Properties", {})
        .get("filterMinmaxSingleComponentFormats")
        is True
    )
    image3d = caps["features"]["sparseResidencyImage3D"] is True
    block3d = sparse_property(caps, "residencyStandard3DBlockShape") is True
    if not tier1:
        tier = "D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED"
    elif not (residency and min_lod and aligned and non_resident and filter_minmax):
        tier = "D3D12_TILED_RESOURCES_TIER_1"
    elif image3d and block3d:
        tier = "D3D12_TILED_RESOURCES_TIER_3"
    else:
        tier = "D3D12_TILED_RESOURCES_TIER_2"
    return {
        "tier": tier,
        "source": source,
        "tier1": features,
        "tier2": {
            "shaderResourceResidency": residency,
            "shaderResourceMinLod": min_lod,
            "residencyAlignedMipSize_false": aligned,
            "residencyNonResidentStrict": non_resident,
            "filterMinmaxSingleComponentFormats": filter_minmax,
        },
        "meetsTier2": tier in {
            "D3D12_TILED_RESOURCES_TIER_2",
            "D3D12_TILED_RESOURCES_TIER_3",
        },
    }


def evaluate_feature_level_implication(p19, caps, version, device_status):
    source = (
        f"v{version} libs/vkd3d/device.c d3d12_device_caps_init_feature_level "
        "(starts at 11_0; 11_1 needs logicOp + vertexPipelineStoresAndAtomics + "
        f"UAV slots>={D3D12_UAV_SLOT_COUNT}; 12_0 needs FL11_1 + tiled tier>=2 + "
        "resource binding tier>=2 + TypedUAVLoadAdditionalFormats; 12_1 needs ROVs + "
        "conservative raster; 12_2 needs SM6.5, VRS, mesh, raytracing, tiled tier 3)"
    )
    logic_op = p19.feature_value(caps, "logicOp") is True
    vs_uav = p19.feature_value(caps, "vertexPipelineStoresAndAtomics") is True
    storage_buffers = p19.uab_value(caps, "maxPerStageDescriptorStorageBuffers")
    if storage_buffers is None:
        storage_buffers = caps.get("coreProperties", {}).get("limits", {}).get(
            "maxPerStageDescriptorStorageBuffers"
        )
        if isinstance(storage_buffers, str):
            storage_buffers = int.from_bytes(bytes.fromhex(storage_buffers), "little")
    storage_images = p19.uab_value(caps, "maxPerStageDescriptorStorageImages")
    if storage_images is None:
        storage_images = caps.get("coreProperties", {}).get("limits", {}).get(
            "maxPerStageDescriptorStorageImages"
        )
        if isinstance(storage_images, str):
            storage_images = int.from_bytes(bytes.fromhex(storage_images), "little")
    tiled = tiled_resources_tier(caps)
    rov_pixel = p19.feature_value(caps, "fragmentShaderPixelInterlock") is True
    rov_sample = p19.feature_value(caps, "fragmentShaderSampleInterlock") is True
    conservative = p19.extension_present(caps, "VK_EXT_conservative_rasterization") is not None
    mesh = p19.extension_present(caps, "VK_EXT_mesh_shader") is not None
    ray = p19.extension_present(caps, "VK_KHR_ray_tracing_pipeline") is not None
    vrs = p19.extension_present(caps, "VK_KHR_fragment_shading_rate") is not None
    shader_output_vp = p19.feature_value(caps, "shaderOutputViewportIndex") is True
    shader_output_layer = p19.feature_value(caps, "shaderOutputLayer") is True
    fl11_1 = (
        logic_op
        and vs_uav
        and storage_buffers is not None
        and storage_buffers >= D3D12_UAV_SLOT_COUNT
        and storage_images is not None
        and storage_images >= D3D12_UAV_SLOT_COUNT
    )
    fl12_0 = fl11_1 and tiled["meetsTier2"]
    fl12_1 = fl12_0 and rov_pixel and rov_sample and conservative
    fl12_2 = (
        fl12_1
        and mesh
        and ray
        and vrs
        and shader_output_vp
        and shader_output_layer
        and tiled["tier"] == "D3D12_TILED_RESOURCES_TIER_3"
    )
    implied = "D3D_FEATURE_LEVEL_11_0"
    if fl11_1:
        implied = "D3D_FEATURE_LEVEL_11_1"
    if fl12_0:
        implied = "D3D_FEATURE_LEVEL_12_0"
    if fl12_1:
        implied = "D3D_FEATURE_LEVEL_12_1"
    if fl12_2:
        implied = "D3D_FEATURE_LEVEL_12_2"
    rows = [
        requirement_row(
            p19,
            "logicOp",
            p19.feature_value(caps, "logicOp"),
            True,
            version,
            source + " / FL11_1 OutputMergerLogicOp",
            "PASS" if logic_op else "FAIL",
        ),
        requirement_row(
            p19,
            "vertexPipelineStoresAndAtomics",
            p19.feature_value(caps, "vertexPipelineStoresAndAtomics"),
            True,
            version,
            source + " / FL11_1",
            "PASS" if vs_uav else "FAIL",
        ),
        requirement_row(
            p19,
            "maxPerStageDescriptorStorageBuffers",
            storage_buffers,
            D3D12_UAV_SLOT_COUNT,
            version,
            source + " / FL11_1 D3D12_UAV_SLOT_COUNT",
            "PASS" if storage_buffers is not None and storage_buffers >= D3D12_UAV_SLOT_COUNT else "FAIL",
        ),
        requirement_row(
            p19,
            "maxPerStageDescriptorStorageImages",
            storage_images,
            D3D12_UAV_SLOT_COUNT,
            version,
            source + " / FL11_1 D3D12_UAV_SLOT_COUNT",
            "PASS" if storage_images is not None and storage_images >= D3D12_UAV_SLOT_COUNT else "FAIL",
        ),
        requirement_row(
            p19,
            "sparseBinding",
            caps["features"]["sparseBinding"],
            True,
            version,
            source + " / FL12_0 tiled tier>=2",
            "PASS" if caps["features"]["sparseBinding"] else "FAIL",
        ),
        requirement_row(
            p19,
            "sparseResidencyBuffer",
            caps["features"]["sparseResidencyBuffer"],
            True,
            version,
            source + " / FL12_0 tiled tier>=2",
            "PASS" if caps["features"]["sparseResidencyBuffer"] else "FAIL",
        ),
        requirement_row(
            p19,
            "sparseResidencyImage2D",
            caps["features"]["sparseResidencyImage2D"],
            True,
            version,
            source + " / FL12_0 tiled tier>=2",
            "PASS" if caps["features"]["sparseResidencyImage2D"] else "FAIL",
        ),
        requirement_row(
            p19,
            "sparseResidencyAliased",
            caps["features"]["sparseResidencyAliased"],
            True,
            version,
            source + " / FL12_0 tiled tier>=2",
            "PASS" if caps["features"]["sparseResidencyAliased"] else "FAIL",
        ),
        requirement_row(
            p19,
            "shaderResourceResidency",
            caps["features"]["shaderResourceResidency"],
            True,
            version,
            source + " / FL12_0 tiled tier>=2",
            "PASS" if caps["features"]["shaderResourceResidency"] else "FAIL",
        ),
        requirement_row(
            p19,
            "shaderResourceMinLod",
            caps["features"]["shaderResourceMinLod"],
            True,
            version,
            source + " / FL12_0 tiled tier>=2",
            "PASS" if caps["features"]["shaderResourceMinLod"] else "FAIL",
        ),
        requirement_row(
            p19,
            "VK_QUEUE_SPARSE_BINDING_BIT",
            sparse_queue_present(caps),
            True,
            version,
            source + " / FL12_0 tiled queue",
            "PASS" if sparse_queue_present(caps) else "FAIL",
        ),
        requirement_row(
            p19,
            "VK_EXT_fragment_shader_interlock",
            p19.extension_present(caps, "VK_EXT_fragment_shader_interlock"),
            1,
            version,
            source + " / FL12_1 ROVs",
            "PASS" if p19.extension_present(caps, "VK_EXT_fragment_shader_interlock") else "FAIL",
        ),
        requirement_row(
            p19,
            "VK_EXT_mesh_shader",
            p19.extension_present(caps, "VK_EXT_mesh_shader"),
            1,
            version,
            source + " / FL12_2 MeshShaderTier",
            "PASS" if mesh else "FAIL",
        ),
        requirement_row(
            p19,
            "VK_KHR_ray_tracing_pipeline",
            p19.extension_present(caps, "VK_KHR_ray_tracing_pipeline"),
            1,
            version,
            source + " / FL12_2 RaytracingTier",
            "PASS" if ray else "FAIL",
        ),
        requirement_row(
            p19,
            "VK_KHR_fragment_shading_rate",
            p19.extension_present(caps, "VK_KHR_fragment_shading_rate"),
            1,
            version,
            source + " / FL12_2 VariableShadingRateTier",
            "PASS" if vrs else "FAIL",
        ),
    ]
    stock_level = "NOT_AVAILABLE"
    reason = (
        "Stock D3D_FEATURE_LEVEL is not available because stock vkd3d never reaches "
        "CheckFeatureSupport on this ICD. The implied level below is a machine evaluation "
        "of d3d12_device_caps_init_feature_level against advertised Vulkan caps. It is not "
        "a stock CheckFeatureSupport result and must not be merged with HARD_REQUIREMENTS, "
        "PROFILE_BASELINE, or DEVICE_CREATE."
    )
    if device_status != "PASS":
        reason = (
            "D3D_FEATURE_LEVEL is not available because DEVICE_CREATE fails. "
            + reason
        )
    else:
        reason = (
            "DEVICE_CREATE would not return E_INVALIDARG on captured 2.0 timeline-semaphore "
            "bits, but stock vkd3d still cannot legally run on this Android ICD. "
            + reason
        )
    return {
        "status": "NOT_AVAILABLE",
        "reason": reason,
        "d3dFeatureLevel": stock_level,
        "impliedFeatureLevel": implied,
        "impliedIfDeviceCreated": implied,
        "fl11_0": "DEFAULT",
        "fl11_1": "PASS" if fl11_1 else "FAIL",
        "fl12_0": "PASS" if fl12_0 else "FAIL",
        "fl12_1": "PASS" if fl12_1 else "FAIL",
        "fl12_2": "PASS" if fl12_2 else "FAIL",
        "tiledResourcesTier": tiled["tier"],
        "source": source,
        "requirements": rows,
        "blockers": [row["requirement"] for row in rows if row["status"] == "FAIL"],
    }


def evaluate_version_2_0(p19, caps):
    hard = evaluate_hard_2_0(p19, caps)
    device = evaluate_device_create_2_0(p19, caps)
    feature_level = evaluate_feature_level_implication(p19, caps, "2.0", device["status"])
    return {
        "vkd3dVersion": "2.0",
        "tag": "v2.0",
        "repository": VKD3D_REPO,
        "sourceKind": "source-derived",
        "sourceFile": str(REQ_2_0.relative_to(ROOT)),
        "sourceProfile": None,
        "source": f"{VKD3D_REPO}/blob/v2.0/README.md",
        "HARD_REQUIREMENTS": hard,
        "PROFILE_BASELINE": {
            "status": "NOT_APPLICABLE",
            "label": "no official VP_D3D12_VKD3D_PROTON_profile.json at v2.0",
            "passCount": 0,
            "failCount": 0,
            "requirements": [],
            "blockers": [],
            "source": "v2.0 has no official Vulkan profile; PROFILE_BASELINE is not a 2.0 gate",
        },
        "DEVICE_CREATE": device,
        "FEATURE_LEVEL": feature_level,
        "stockVkd3dSmoke": "BLOCKED",
        "smokeReason": device["smokeReason"],
    }


def evaluate_version_later(p19, evaluator, caps, version):
    hard = p19.evaluate_hard(caps, version)
    device = evaluate_device_create_later(p19, caps, version)
    evaluated_levels = {
        name: p19.evaluate_profile(caps, evaluator, version, name)
        for name in FEATURE_LEVEL_PROFILES
    }
    baseline = evaluated_levels[BASELINE_PROFILE]
    for row in baseline["requirements"]:
        extra = P21_CLASSIFICATION.get(row["requirement"])
        if extra and row["status"] != "PASS":
            row.update(extra)
    feature_level = evaluate_feature_level_implication(p19, caps, version, device["status"])
    feature_level["profiles"] = {
        name: {
            "status": item["status"],
            "failCount": item["failCount"],
            "passCount": item["passCount"],
            "blockers": item["blockers"],
            "label": item["label"],
        }
        for name, item in evaluated_levels.items()
    }
    feature_level["incrementalBlockers"] = {
        name: item["blockers"] for name, item in evaluated_levels.items()
    }
    return {
        "vkd3dVersion": version,
        "tag": f"v{version}",
        "repository": VKD3D_REPO,
        "sourceKind": "official-profile",
        "sourceFile": str(p19.PROFILES[version].relative_to(ROOT)),
        "sourceProfile": BASELINE_PROFILE,
        "source": json.loads(p19.PROFILES[version].read_text())["provenance"]["url"],
        "provenance": json.loads(p19.PROFILES[version].read_text())["provenance"],
        "HARD_REQUIREMENTS": hard,
        "PROFILE_BASELINE": baseline,
        "DEVICE_CREATE": device,
        "FEATURE_LEVEL": feature_level,
        "stockVkd3dSmoke": "BLOCKED",
        "smokeReason": device["smokeReason"],
        "vendorMaximumProfilesOutOfScope": list(VENDOR_PROFILES),
    }


def md_table(rows, failures_only=False):
    selected = [row for row in rows if (row["status"] == "FAIL") or not failures_only]
    if not selected:
        return "None.\n"
    lines = [
        "| Requirement | Current | Required | Status | Source version | Source file/profile | Upstream PanVK | Implementation | Action |",
        "|---|---:|---:|---|---|---|---|---|---|",
    ]
    for row in selected:
        lines.append(
            "| `{requirement}` | `{current}` | `{required}` | {status} | `{sourceVersion}` | `{source}` | {upstreamPanVK} | {implementationStatus} | {action} |".format(
                **row
            )
        )
    return "\n".join(lines) + "\n"


def render_md(doc):
    driver = doc["driver"]
    v20 = doc["versions"]["2.0"]
    v214 = doc["versions"]["2.14.1"]
    v301 = doc["versions"]["3.0.1"]
    lines = [
        "# P21 D3D12 feature/profile analysis",
        "",
        "## Result",
        "",
        f"`{doc['result']}`",
        "",
        "Machine-evaluated HARD_REQUIREMENTS, PROFILE_BASELINE, DEVICE_CREATE, and FEATURE_LEVEL only.",
        "These four gates stay separate. No merged PASS. No PanVK patch. No feature-bit change.",
        "No sparse implementation. P22 was not started.",
        "",
        "```text",
        f"VKD3D_2_0_HARD={v20['HARD_REQUIREMENTS']['status']}",
        f"VKD3D_2_0_BASELINE={v20['PROFILE_BASELINE']['status']}",
        f"VKD3D_2_0_DEVICE={v20['DEVICE_CREATE']['status']}",
        f"VKD3D_2_0_FEATURE_LEVEL={v20['FEATURE_LEVEL']['d3dFeatureLevel']}",
        f"VKD3D_2_0_IMPLIED={v20['FEATURE_LEVEL']['impliedFeatureLevel']}",
        f"VKD3D_2_0_SMOKE={v20['stockVkd3dSmoke']}",
        f"VKD3D_2_14_1_HARD={v214['HARD_REQUIREMENTS']['status']}",
        f"VKD3D_2_14_1_BASELINE={v214['PROFILE_BASELINE']['status']}",
        f"VKD3D_2_14_1_DEVICE={v214['DEVICE_CREATE']['status']}",
        f"VKD3D_2_14_1_FEATURE_LEVEL={v214['FEATURE_LEVEL']['d3dFeatureLevel']}",
        f"VKD3D_2_14_1_IMPLIED={v214['FEATURE_LEVEL']['impliedFeatureLevel']}",
        f"VKD3D_2_14_1_SMOKE={v214['stockVkd3dSmoke']}",
        f"VKD3D_3_0_1_HARD={v301['HARD_REQUIREMENTS']['status']}",
        f"VKD3D_3_0_1_BASELINE={v301['PROFILE_BASELINE']['status']}",
        f"VKD3D_3_0_1_DEVICE={v301['DEVICE_CREATE']['status']}",
        f"VKD3D_3_0_1_FEATURE_LEVEL={v301['FEATURE_LEVEL']['d3dFeatureLevel']}",
        f"VKD3D_3_0_1_IMPLIED={v301['FEATURE_LEVEL']['impliedFeatureLevel']}",
        f"VKD3D_3_0_1_SMOKE={v301['stockVkd3dSmoke']}",
        "```",
        "",
        "## Adapter",
        "",
        f"- Device: `{driver['adapter']}`",
        f"- Target: `{doc['capture']['target']}`",
        f"- VendorID: `{driver['vendorID']}`",
        f"- DeviceID: `{driver['deviceIDHex']}`",
        f"- Driver: `{driver['driverName']}` / `{driver['driverInfo']}`",
        f"- DriverID: `{driver['driverID']}` (`VK_DRIVER_ID_MESA_PANVK`)",
        f"- DriverVersion packed: `{driver['driverVersionPacked']}` (`{driver['driverVersionHex']}`)",
        f"- VK_VERSION decode: `{driver['driverVersionVk']['major']}.{driver['driverVersionVk']['minor']}.{driver['driverVersionVk']['patch']}` (packed git encoding; `driverInfo` is authoritative)",
        f"- Vulkan API: `{driver['apiVersionText']}`",
        f"- ICD SHA-256: `{doc['capture']['icdSha256']}`",
        f"- Capture: `{doc['capture']['command']}` at `{doc['capture']['timestamp']}`",
        "",
        "## Gate separation",
        "",
        "HARD_REQUIREMENTS comes from each tag's README. PROFILE_BASELINE comes from",
        f"`{BASELINE_PROFILE}` in `VP_D3D12_VKD3D_PROTON_profile.json` for 2.14.1 and 3.0.1.",
        "vkd3d-proton 2.0 has no official profile, so PROFILE_BASELINE is `NOT_APPLICABLE`.",
        "DEVICE_CREATE comes from `vkd3d_init_device_caps` E_INVALIDARG gates in",
        "`libs/vkd3d/device.c`. FEATURE_LEVEL is the D3D12 feature level that stock vkd3d",
        "would report after a successful device create. Implied FL is a separate machine",
        "evaluation of `d3d12_device_caps_init_feature_level` and must not be treated as",
        "stock `CheckFeatureSupport`.",
        "",
        "README HARD can PASS while DEVICE_CREATE FAILs. PROFILE_BASELINE is broader than",
        "either gate. Implied FL 11_0 is the vkd3d default after a successful create; G615",
        "cannot climb because `vertexPipelineStoresAndAtomics` is false (FL 11_1) and sparse",
        "tiled resources are false (FL 12_0). P22 sparse/Kbase was not started.",
        "",
        "## Initialization / stock vkd3d smoke",
        "",
        "Stock vkd3d-proton tags `v2.0`, `v2.14.1`, and `v3.0.1` are Windows D3D12-on-Vulkan",
        "layers. This Android ICD is not a legal host. Smoke is `BLOCKED` for all three.",
        "v2.14.1 and v3.0.1 would also fail `vkd3d_init_device_caps` on `robustImageAccess2`",
        "and `transformFeedbackQueries`. No game boot, no vkd3d init log, no reported D3D",
        "feature level from stock `CheckFeatureSupport`.",
        "",
        "P6 left `robustImageAccess2` false. P9-P17 left geometry, tessellation, XFB, BC,",
        "clip/cull, fillModeNonSolid, and multiViewport false. P20 left",
        "`pipelineStatisticsQuery` false. P21 did not flip them and did not implement sparse.",
        "",
    ]
    for version in ("2.0", "2.14.1", "3.0.1"):
        item = doc["versions"][version]
        hard = item["HARD_REQUIREMENTS"]
        baseline = item["PROFILE_BASELINE"]
        device = item["DEVICE_CREATE"]
        feature_level = item["FEATURE_LEVEL"]
        lines += [
            f"## vkd3d-proton {version}",
            "",
            f"HARD_REQUIREMENTS: **{hard['status']}**. PROFILE_BASELINE: **{baseline['status']}**. DEVICE_CREATE: **{device['status']}**. FEATURE_LEVEL: **{feature_level['d3dFeatureLevel']}**. Implied: **{feature_level['impliedFeatureLevel']}**. Smoke: **{item['stockVkd3dSmoke']}**.",
            "",
            f"Source: `{item['source']}` (`{item.get('sourceFile')}`).",
            "",
            "### HARD_REQUIREMENTS",
            "",
            f"Source: `{hard['source']}`",
            "",
            hard.get("note", ""),
            "",
            f"Pass `{hard['passCount']}` / fail `{hard['failCount']}`.",
            "",
            md_table(hard["requirements"]),
        ]
        if hard.get("recommended"):
            lines += ["Highly recommended, not mandatory:", "", md_table(hard["recommended"])]
        lines += [
            "### PROFILE_BASELINE",
            "",
        ]
        if baseline["status"] == "NOT_APPLICABLE":
            lines += [baseline.get("source", "Not applicable."), ""]
        else:
            lines += [
                f"`{BASELINE_PROFILE}` (`{baseline.get('label')}`) is the official minimum D3D12 baseline.",
                "",
                f"Pass `{baseline['passCount']}` / fail `{baseline['failCount']}`.",
                "",
                md_table(baseline["requirements"], failures_only=True),
            ]
        lines += [
            "### DEVICE_CREATE",
            "",
            f"Source: `{device['source']}`",
            "",
            device["smokeReason"],
            "",
            md_table(device["requirements"]),
            "### FEATURE_LEVEL",
            "",
            feature_level["reason"],
            "",
            f"Stock `CheckFeatureSupport` D3D_FEATURE_LEVEL: `{feature_level['d3dFeatureLevel']}`.",
            f"Implied `d3d12_device_caps_init_feature_level`: `{feature_level['impliedFeatureLevel']}`.",
            f"Tiled resources tier: `{feature_level.get('tiledResourcesTier')}`.",
            "",
            f"FL11_0 default: `{feature_level.get('fl11_0')}`. FL11_1: `{feature_level.get('fl11_1')}`. FL12_0: `{feature_level.get('fl12_0')}`. FL12_1: `{feature_level.get('fl12_1')}`. FL12_2: `{feature_level.get('fl12_2')}`.",
            "",
            md_table(feature_level["requirements"], failures_only=True),
        ]
        if "profiles" in feature_level:
            lines += [
                "Official FL profiles remain FAIL and are not a D3D feature level:",
                "",
                "| Profile | Status | Pass | Fail |",
                "|---|---|---:|---:|",
            ]
            for name in FEATURE_LEVEL_PROFILES:
                fl = feature_level["profiles"][name]
                lines.append(
                    f"| `{name}` | {fl['status']} | {fl['passCount']} | {fl['failCount']} |"
                )
            lines += [
                "",
                "Vendor-maximum profiles `VP_D3D12_maximum_radv` and `VP_D3D12_maximum_nv` are not D3D feature levels.",
                "",
            ]
    extras = doc["p20Extras"]
    lines += [
        "## Remaining blockers",
        "",
        "| Feature | Gate | Owner | Implementation |",
        "|---|---|---|---|",
        "| `robustImageAccess2` | DEVICE_CREATE + PROFILE_BASELINE | P6 | `UNSUPPORTED` |",
        "| `transformFeedbackQueries` | DEVICE_CREATE (2.14.1/3.0.1) + PROFILE_BASELINE | P15 | `BLOCKED_SAFE_FALSE` |",
        "| `VK_EXT_transform_feedback` | PROFILE_BASELINE | P15 | `BLOCKED_SAFE_FALSE` |",
        "| `transformFeedback` | PROFILE_BASELINE | P15 | `BLOCKED_SAFE_FALSE` |",
        "| `geometryStreams` | PROFILE_BASELINE | P15 | `BLOCKED_SAFE_FALSE` |",
        "| `geometryShader` | PROFILE_BASELINE | P12 | `BLOCKED_SAFE_FALSE` |",
        "| `fillModeNonSolid` | PROFILE_BASELINE | P11 | `BLOCKED_SAFE_FALSE` |",
        "| `shaderClipDistance` | PROFILE_BASELINE | P10 | `BLOCKED_SAFE_FALSE` |",
        "| `shaderCullDistance` | PROFILE_BASELINE | P10 | `BLOCKED_SAFE_FALSE` |",
        "| `textureCompressionBC` | PROFILE_BASELINE | P9 | `BLOCKED_SAFE_FALSE` (`native=false`, `emulated=false`) |",
        "| `multiViewport` | PROFILE_BASELINE | P14 | `BLOCKED_SAFE_FALSE` |",
        "| `tessellationShader` | PROFILE_BASELINE | P17 | `BLOCKED_SAFE_FALSE` |",
        f"| `pipelineStatisticsQuery` | PROFILE_BASELINE | P20 | `{extras['p20Owned']['pipelineStatisticsQuery']['implementationStatus']}` |",
        "| `vertexPipelineStoresAndAtomics` | FEATURE_LEVEL 11_1 | later | `NOT_IMPLEMENTED` |",
        "| `sparseBinding` / tiled resources | FEATURE_LEVEL 12_0 | P22 | `NOT_IMPLEMENTED` (not started) |",
        "| `bufferImageGranularity` | PROFILE_BASELINE evaluator | n/a | `EVALUATOR_DIRECTION` (actual 64 is finer than 65536) |",
        "| `subgroupSupportedOperations[0]` | PROFILE_BASELINE evaluator | n/a | `EVALUATOR_ARRAY` (bitmask 1791 includes BASIC) |",
        "",
        "## Checks",
        "",
        "- Official tagged profiles `validation/g615-v11-csf/profiles/vkd3d-2.14.1.json` and `vkd3d-3.0.1.json`.",
        "- Source-derived `validation/requirements/vkd3d-proton-2.0.json`.",
        "- Tagged README hard-device requirements for `v2.0`, `v2.14.1`, `v3.0.1`.",
        "- Tagged `libs/vkd3d/device.c` `vkd3d_init_device_caps` and `d3d12_device_caps_init_feature_level`.",
        "- Existing evaluators `scripts/evaluate-vkd3d-profile-baseline.py` and `scripts/evaluate-consumer-profile.py`.",
        "- Capability dump `validation/g615-v11-csf/consumer-capabilities.json`.",
        "- P19/P20 extras `validation/g615-v11-csf/p20-remaining-baseline-extras.json`.",
        "- `tests/dxvk-vkd3d/test_p21_d3d12_feature_level.py`.",
        "",
        "## Exit",
        "",
        "P21 exits `FAIL`. Analysis complete. No missing feature was implemented. No bit was flipped.",
        "HARD_REQUIREMENTS PASSes for 2.0, 2.14.1, and 3.0.1. PROFILE_BASELINE is `NOT_APPLICABLE` for 2.0 and FAILs for 2.14.1/3.0.1.",
        "DEVICE_CREATE PASSes the 2.0 timeline-semaphore gate and FAILs 2.14.1/3.0.1 on `robustImageAccess2` and `transformFeedbackQueries`.",
        "Stock FEATURE_LEVEL is `NOT_AVAILABLE`. Implied FL is `D3D_FEATURE_LEVEL_11_0` and cannot climb.",
        "Stock vkd3d smoke is `BLOCKED`. Next engineering step is not P22 sparse; DEVICE_CREATE stays blocked until `robustImageAccess2` and XFB queries are real.",
        "",
    ]
    return "\n".join(lines)


def render_txt(doc):
    lines = [
        "P21 D3D12 feature/profile analysis",
        f"result={doc['result']}",
        f"adapter={doc['driver']['adapter']}",
        f"driver={doc['driver']['driverName']} {doc['driver']['driverInfo']}",
        f"api={doc['driver']['apiVersionText']}",
        f"icd={doc['capture']['icdSha256']}",
    ]
    for version, item in doc["versions"].items():
        lines.append(
            f"vkd3d-{version} HARD={item['HARD_REQUIREMENTS']['status']} "
            f"BASELINE={item['PROFILE_BASELINE']['status']} "
            f"DEVICE={item['DEVICE_CREATE']['status']} "
            f"FEATURE_LEVEL={item['FEATURE_LEVEL']['d3dFeatureLevel']} "
            f"IMPLIED={item['FEATURE_LEVEL']['impliedFeatureLevel']} "
            f"smoke={item['stockVkd3dSmoke']}"
        )
        lines.append(
            f"  HARD fail={item['HARD_REQUIREMENTS']['failCount']} blockers={','.join(item['HARD_REQUIREMENTS']['blockers']) or 'none'}"
        )
        lines.append(
            f"  BASELINE fail={item['PROFILE_BASELINE'].get('failCount', 0)} blockers={','.join(item['PROFILE_BASELINE'].get('blockers') or []) or item['PROFILE_BASELINE']['status']}"
        )
        lines.append(
            f"  DEVICE fail={item['DEVICE_CREATE']['failCount']} blockers={','.join(item['DEVICE_CREATE']['blockers']) or 'none'}"
        )
        lines.append(
            f"  IMPLIED={item['FEATURE_LEVEL']['impliedFeatureLevel']} tiled={item['FEATURE_LEVEL'].get('tiledResourcesTier')} blockers={','.join(item['FEATURE_LEVEL'].get('blockers') or [])}"
        )
        lines.append(f"  {item['smokeReason']}")
    lines.append("P21_EXIT=FAIL")
    return "\n".join(lines) + "\n"


def main():
    p19 = load_p19()
    evaluator = p19.load_evaluator()
    caps = json.loads(CAPS_PATH.read_text())
    extras = json.loads(P20_EXTRAS.read_text())
    versions = {
        "2.0": evaluate_version_2_0(p19, caps),
        "2.14.1": evaluate_version_later(p19, evaluator, caps, "2.14.1"),
        "3.0.1": evaluate_version_later(p19, evaluator, caps, "3.0.1"),
    }
    doc = {
        "schemaVersion": 1,
        "phase": "P21",
        "date": "2026-09-20",
        "result": "FAIL",
        "smoke": "BLOCKED",
        "d3dFeatureLevel": "NOT_AVAILABLE",
        "impliedFeatureLevel": versions["2.14.1"]["FEATURE_LEVEL"]["impliedFeatureLevel"],
        "pinnedMesa": "5a07217f034b3e50d8c7c7794f97a2df1742613b",
        "auditedMesa": "e1f3f372c4a661cd0e71a0cdafc4d469d56ecf35",
        "capabilities": str(CAPS_PATH.relative_to(ROOT)),
        "evaluator": "scripts/evaluate-vkd3d-d3d12-feature-level.py",
        "capture": caps["capture"],
        "device": caps["device"],
        "driver": p19.driver_identity(caps),
        "p20Extras": extras,
        "featureBitsUnchanged": {name: caps["features"][name] for name in FALSE_BITS},
        "outOfScope": [
            "P22 sparse/Kbase implementation",
            "P23 final matrix",
            "feature-bit flips",
            "missing-feature implementation",
        ],
        "versions": versions,
        "gatesUnmerged": [
            "HARD_REQUIREMENTS",
            "PROFILE_BASELINE",
            "DEVICE_CREATE",
            "FEATURE_LEVEL",
        ],
    }
    JSON_OUT.write_text(json.dumps(doc, indent=2) + "\n")
    MD_OUT.write_text(render_md(doc))
    TXT_OUT.write_text(render_txt(doc))
    print(f"PASS output={JSON_OUT}")
    for version, item in versions.items():
        key = version.replace(".", "_")
        print(
            f"VKD3D_{key}_HARD={item['HARD_REQUIREMENTS']['status']} "
            f"BASELINE={item['PROFILE_BASELINE']['status']} "
            f"DEVICE={item['DEVICE_CREATE']['status']} "
            f"FEATURE_LEVEL={item['FEATURE_LEVEL']['d3dFeatureLevel']} "
            f"IMPLIED={item['FEATURE_LEVEL']['impliedFeatureLevel']} "
            f"SMOKE={item['stockVkd3dSmoke']}"
        )
    print("P21_EXIT=FAIL")


if __name__ == "__main__":
    main()
