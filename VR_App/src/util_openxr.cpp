//
// Created by stand on 30.07.2024.
//
#include <GLES3/gl32.h>
#include "pch.h"
#include "util_egl.h"
#include "check.h"
#include "log.h"

#include "util_openxr.h"

namespace Math::Pose {
    XrPosef Identity() {
        XrPosef t{};
        t.orientation.w = 1;
        return t;
    }

    XrPosef Translation(const XrVector3f &translation) {
        XrPosef t = Identity();
        t.position = translation;
        return t;
    }

    XrPosef RotateCCWAboutYAxis(float radians, XrVector3f translation) {
        XrPosef t = Identity();
        t.orientation.x = 0.f;
        t.orientation.y = std::sin(radians * 0.5f);
        t.orientation.z = 0.f;
        t.orientation.w = std::cos(radians * 0.5f);
        t.position = translation;
        return t;
    }
}

int openxr_init_loader(android_app *app) {

    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE,
                                           "xrInitializeLoaderKHR",
                                           (PFN_xrVoidFunction *) (&initializeLoader)))) {
        XrLoaderInitInfoAndroidKHR loaderInitInfoAndroid;
        memset(&loaderInitInfoAndroid, 0, sizeof(loaderInitInfoAndroid));
        loaderInitInfoAndroid.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
        loaderInitInfoAndroid.next = nullptr;
        loaderInitInfoAndroid.applicationVM = app->activity->vm;
        loaderInitInfoAndroid.applicationContext = app->activity->clazz;
        initializeLoader((const XrLoaderInitInfoBaseHeaderKHR *) &loaderInitInfoAndroid);
    }
    return 0;
}

void openxr_create_instance(android_app *app, XrInstance *instance) {
    openxr_log_layers_and_extensions();

    CHECK(*instance == XR_NULL_HANDLE)

    // Transform platform and graphics extension std::strings to C strings.
    const std::vector<const char *> extensions = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                                  XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
                                                  XR_EXT_USER_PRESENCE_EXTENSION_NAME};

    XrInstanceCreateInfoAndroidKHR instance_create_info = {
            XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    instance_create_info.applicationVM = app->activity->vm;
    instance_create_info.applicationActivity = app->activity->clazz;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &instance_create_info;
    createInfo.enabledExtensionCount = extensions.size();
    createInfo.enabledExtensionNames = extensions.data();

    strncpy(createInfo.applicationInfo.applicationName, "BUT_Telepresence", XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationName[XR_MAX_APPLICATION_NAME_SIZE - 1] = '\0';
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;

    CHECK_XRCMD(xrCreateInstance(&createInfo, instance))

    CHECK(*instance != XR_NULL_HANDLE)

    XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
    CHECK_XRCMD(xrGetInstanceProperties(*instance, &instanceProperties))

    LOG_INFO("Instance RuntimeName=%s RuntimeVersion=%s", instanceProperties.runtimeName,
             GetXrVersionString(instanceProperties.runtimeVersion).c_str());
}

void openxr_log_layers_and_extensions() {
    const auto logExtensions = [](const char *layerName, int indent = 0) {
        uint32_t instanceExtensionCount;
        CHECK_XRCMD(
                xrEnumerateInstanceExtensionProperties(layerName, 0, &instanceExtensionCount,
                                                       nullptr))

        std::vector<XrExtensionProperties> extensions(instanceExtensionCount);
        for (XrExtensionProperties &extension: extensions) {
            extension.type = XR_TYPE_EXTENSION_PROPERTIES;
        }

        CHECK_XRCMD(
                xrEnumerateInstanceExtensionProperties(layerName, (uint32_t) extensions.size(),
                                                       &instanceExtensionCount,
                                                       extensions.data()))

        const std::string indentStr(indent, ' ');
        LOG_INFO("%sAvailable Extensions: (%d)", indentStr.c_str(), instanceExtensionCount);
        for (const XrExtensionProperties &extension: extensions) {
            LOG_INFO("%s  Name=%s SpecVersion=%d", indentStr.c_str(), extension.extensionName,
                     extension.extensionVersion);
        }
    };

    // Log non-layer extensions (layerName==nullptr).
    logExtensions(nullptr);

    // Log layers and any of their extensions.
    {
        uint32_t layerCount;
        CHECK_XRCMD(xrEnumerateApiLayerProperties(0, &layerCount, nullptr))

        std::vector<XrApiLayerProperties> layers(layerCount);
        for (XrApiLayerProperties &layer: layers) {
            layer.type = XR_TYPE_API_LAYER_PROPERTIES;
        }

        CHECK_XRCMD(xrEnumerateApiLayerProperties((uint32_t) layers.size(), &layerCount,
                                                  layers.data()))

        LOG_INFO("Available Layers: (%d)", layerCount);
        for (const XrApiLayerProperties &layer: layers) {
            LOG_INFO("  Name=%s SpecVersion=%s LayerVersion=%d Description=%s", layer.layerName,
                     GetXrVersionString(layer.specVersion).c_str(), layer.layerVersion,
                     layer.description);
            logExtensions(layer.layerName, 4);
        }
    }
}

void openxr_get_system_id(XrInstance *instance, XrSystemId *system_id) {
    CHECK(*instance != XR_NULL_HANDLE)
    CHECK(*system_id == XR_NULL_SYSTEM_ID)

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    CHECK_XRCMD(xrGetSystem(*instance, &systemInfo, system_id))

    LOG_INFO("Using system %llu for form factor %s", (unsigned long long) *system_id,
             to_string(XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY));

    CHECK(*system_id != XR_NULL_SYSTEM_ID)
}

void openxr_confirm_gfx_reqs(XrInstance *instance, XrSystemId *system_id) {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnXrGetOpenGLESGraphicsRequirementsKHR = nullptr;
    CHECK_XRCMD(xrGetInstanceProcAddr(*instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                                      reinterpret_cast<PFN_xrVoidFunction *>(&pfnXrGetOpenGLESGraphicsRequirementsKHR)));
    XrGraphicsRequirementsOpenGLESKHR graphicsRequirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    CHECK_XRCMD(
            pfnXrGetOpenGLESGraphicsRequirementsKHR(*instance, *system_id, &graphicsRequirements));

    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);

    LOG_INFO("Required OpenGL ES Version: %s ~ %s",
             GetXrVersionString(graphicsRequirements.minApiVersionSupported).c_str(),
             GetXrVersionString(graphicsRequirements.maxApiVersionSupported).c_str());

    LOG_INFO("Using OpenGLES %d.%d", major, minor);

    const XrVersion desiredApiVersion = XR_MAKE_VERSION(major, minor, 0);
    if (graphicsRequirements.minApiVersionSupported > desiredApiVersion) {
        THROW("Runtime does not support desired Graphics API and/or version")
    }
}

void openxr_log_reference_spaces(XrSession *session) {
    CHECK(*session != XR_NULL_HANDLE)

    uint32_t spaceCount;
    CHECK_XRCMD(xrEnumerateReferenceSpaces(*session, 0, &spaceCount, nullptr))
    std::vector<XrReferenceSpaceType> spaces(spaceCount);
    CHECK_XRCMD(xrEnumerateReferenceSpaces(*session, spaceCount, &spaceCount, spaces.data()))

    LOG_INFO("Available reference spaces: %d", spaceCount);
    for (auto space: spaces) {
        LOG_INFO("  Name: %s", to_string(space));
    }
}

XrReferenceSpaceCreateInfo openxr_get_reference_space_create_info(std::string reference_space) {
    XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::Identity();
    if (EqualsIgnoreCase(reference_space, "View")) {
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    } else if (EqualsIgnoreCase(reference_space, "ViewFront")) {
        // Render head-locked 2m in front of device.
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::Translation({0.f, 0.f, -2.f}),
                referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    } else if (EqualsIgnoreCase(reference_space, "Local")) {
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    } else if (EqualsIgnoreCase(reference_space, "Stage")) {
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (EqualsIgnoreCase(reference_space, "StageLeft")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::RotateCCWAboutYAxis(0.f,
                                                                                        {-2.f, 0.f,
                                                                                         -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (EqualsIgnoreCase(reference_space, "StageRight")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::RotateCCWAboutYAxis(0.f,
                                                                                        {2.f, 0.f,
                                                                                         -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (EqualsIgnoreCase(reference_space, "StageLeftRotated")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::RotateCCWAboutYAxis(3.14f / 3.f,
                                                                                        {-2.f, 0.5f,
                                                                                         -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (EqualsIgnoreCase(reference_space, "StageRightRotated")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::RotateCCWAboutYAxis(
                -3.14f / 3.f, {2.f, 0.5f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else {
        throw std::invalid_argument(
                Fmt("Unknown reference space type '%s'", reference_space.c_str()));
    }
    return referenceSpaceCreateInfo;
}

void openxr_create_reference_spaces(XrSession *session, std::vector<XrSpace> &reference_spaces) {
    CHECK(*session != XR_NULL_HANDLE)

    std::string visualized_spaces[] = {
            "ViewFront", "Local", "Stage", "StageLeft", "StageRight",
            "StageLeftRotated", "StageRightRotated"
    };

    for (const auto &visualized_space: visualized_spaces) {
        XrReferenceSpaceCreateInfo referenceSpaceCreateInfo
                = openxr_get_reference_space_create_info(visualized_space);
        XrSpace space;
        XrResult res = xrCreateReferenceSpace(*session, &referenceSpaceCreateInfo, &space);
        if (XR_SUCCEEDED(res)) {
            reference_spaces.push_back(space);
        } else {
            LOG_INFO("Failed to create reference space %s with error %d", visualized_space.c_str(),
                     res);
        }
    }
}

std::vector<XrViewConfigurationView>
openxr_enumerate_view_configurations(XrInstance *instance, XrSystemId *system_id) {
    CHECK(*instance != XR_NULL_HANDLE)
    CHECK(*system_id != XR_NULL_SYSTEM_ID)

    uint32_t viewConfigTypeCount;
    CHECK_XRCMD(xrEnumerateViewConfigurations(*instance, *system_id, 0, &viewConfigTypeCount,
                                              nullptr))
    std::vector<XrViewConfigurationType> viewConfigTypes(viewConfigTypeCount);
    CHECK_XRCMD(xrEnumerateViewConfigurations(*instance, *system_id, viewConfigTypeCount,
                                              &viewConfigTypeCount,
                                              viewConfigTypes.data()))
    CHECK((uint32_t) viewConfigTypes.size() == viewConfigTypeCount)

    LOG_INFO("Available View Configuration Types: (%d)", viewConfigTypeCount);

    std::vector<XrViewConfigurationView> returnConfigurations;

    for (auto viewConfigType: viewConfigTypes) {
        LOG_INFO("  View Configuration Type: %s %s", to_string(viewConfigType),
                 viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO ? "(Selected)" : "");

        XrViewConfigurationProperties viewConfigProperties{XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
        CHECK_XRCMD(xrGetViewConfigurationProperties(*instance, *system_id, viewConfigType,
                                                     &viewConfigProperties))

        LOG_INFO("  View Configuration FovMutable=%s",
                 viewConfigProperties.fovMutable == XR_TRUE ? "True" : "False");

        uint32_t viewCount;
        CHECK_XRCMD(xrEnumerateViewConfigurationViews(*instance, *system_id, viewConfigType, 0,
                                                      &viewCount,
                                                      nullptr))
        if (viewCount > 0) {
            std::vector<XrViewConfigurationView> views(viewCount,
                                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW});
            CHECK_XRCMD(
                    xrEnumerateViewConfigurationViews(*instance, *system_id, viewConfigType,
                                                      viewCount,
                                                      &viewCount,
                                                      views.data()))


            for (uint32_t i = 0; i < views.size(); i++) {
                const XrViewConfigurationView &view = views[i];
                LOG_INFO("    View [%d]: Recommended Width=%d Height=%d SampleCount=%d", i,
                         view.recommendedImageRectWidth, view.recommendedImageRectHeight,
                         view.recommendedSwapchainSampleCount);
                LOG_INFO("    View [%d]:   Maximum Width=%d Height=%d SampleCount=%d", i,
                         view.maxImageRectWidth, view.maxImageRectHeight,
                         view.maxSwapchainSampleCount);
            }

            if (viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
                returnConfigurations = views;
            }
        } else {
            LOG_ERROR("Empty view configuration type");
        }

        openxr_log_environment_blend_modes(instance, system_id, viewConfigType);
    }

    return returnConfigurations;
}

void openxr_log_environment_blend_modes(XrInstance *instance, XrSystemId *system_id,
                                        XrViewConfigurationType type) {
    CHECK(*instance != XR_NULL_HANDLE)
    CHECK(*system_id != XR_NULL_SYSTEM_ID)

    uint32_t count;
    CHECK_XRCMD(xrEnumerateEnvironmentBlendModes(*instance, *system_id, type, 0, &count,
                                                 nullptr))
    CHECK(count > 0)

    LOG_INFO("Available Environment Blend Mode count: (%u)", count);

    std::vector<XrEnvironmentBlendMode> blendModes(count);
    CHECK_XRCMD(xrEnumerateEnvironmentBlendModes(*instance, *system_id, type, count, &count,
                                                 blendModes.data()))
    bool blendModeFound = false;
    for (auto mode: blendModes) {
        const bool blendModeMatch = (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
        LOG_INFO("Environment Blend Mode (%s): %s", to_string(mode),
                 blendModeMatch ? "(Selected)" : "");
        blendModeFound |= blendModeMatch;
    }
    CHECK(blendModeFound)
}

std::vector<viewsurface_t>
openxr_create_swapchains(XrInstance *instance, XrSystemId *system_id, XrSession *session) {
    // Read graphics properties for preferred swapchain length and logging
    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
    CHECK_XRCMD(xrGetSystemProperties(*instance, *system_id, &systemProperties))

    // Log system properties
    LOG_INFO("System Properties: Name=%s VendorId=%d", systemProperties.systemName,
             systemProperties.vendorId);
    LOG_INFO("System Graphics Properties: MaxWidth=%d MaxHeight=%d MaxLayers=%d",
             systemProperties.graphicsProperties.maxSwapchainImageWidth,
             systemProperties.graphicsProperties.maxSwapchainImageHeight,
             systemProperties.graphicsProperties.maxLayerCount);
    LOG_INFO("System Tracking Properties: OrientationTracking=%s PositionTracking=%s",
             systemProperties.trackingProperties.orientationTracking == XR_TRUE ? "True"
                                                                                : "False",
             systemProperties.trackingProperties.positionTracking == XR_TRUE ? "True"
                                                                             : "False");

    std::vector<XrViewConfigurationView> config_views = openxr_enumerate_view_configurations(
            instance, system_id);

    std::vector<viewsurface_t> viewsurfaces;
    viewsurfaces.resize(config_views.size());

    uint32_t swapchainFormatCount;
    CHECK_XRCMD(xrEnumerateSwapchainFormats(*session, 0, &swapchainFormatCount, nullptr))
    std::vector<int64_t> swapchainFormats(swapchainFormatCount);
    CHECK_XRCMD(xrEnumerateSwapchainFormats(*session, swapchainFormatCount,
                                            &swapchainFormatCount,
                                            swapchainFormats.data()))

    // Print swapchain formats and the selected one
    {
        std::string swapchainFormatsString;
        for (int64_t format: swapchainFormats) {
            const bool selected = format == GL_RGBA8;
            swapchainFormatsString += " ";
            if (selected) {
                swapchainFormatsString += "[";
            }
            swapchainFormatsString += std::to_string(format);
            if (selected) {
                swapchainFormatsString += "]";
            }
        }
        LOG_INFO("Swapchain Formats: %s", swapchainFormatsString.c_str());
    }

    // Create a swapchain for each view.
    uint8_t i = 0;
    for (auto &config_view: config_views) {
        LOG_INFO(
                "Creating swapchain for view %d with dimensions Width=%d Height=%d SampleCount=%d",
                i, config_view.recommendedImageRectHeight, config_view.recommendedImageRectHeight,
                config_view.recommendedSwapchainSampleCount);

        // Create a swapchain
        XrSwapchainCreateInfo swapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchainCreateInfo.arraySize = 1;
        swapchainCreateInfo.format = GL_RGBA8;
        swapchainCreateInfo.width = config_view.recommendedImageRectWidth;
        swapchainCreateInfo.height = config_view.recommendedImageRectHeight;
        swapchainCreateInfo.mipCount = 1;
        swapchainCreateInfo.faceCount = 1;
        swapchainCreateInfo.sampleCount = config_view.recommendedSwapchainSampleCount;
        swapchainCreateInfo.usageFlags =
                XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

        viewsurfaces[i].width = static_cast<int32_t>(swapchainCreateInfo.width);
        viewsurfaces[i].height = static_cast<int32_t>(swapchainCreateInfo.height);
        CHECK_XRCMD(xrCreateSwapchain(*session, &swapchainCreateInfo, &viewsurfaces[i].swapchain))
        openxr_allocate_swapchain_rendertargets(viewsurfaces[i]);

        i++;
    }

    return viewsurfaces;
}

void openxr_allocate_swapchain_rendertargets(viewsurface_t &viewsurface) {
    uint32_t imageCount;
    CHECK_XRCMD(xrEnumerateSwapchainImages(viewsurface.swapchain, 0, &imageCount, nullptr))
    auto *swapchain_images = (XrSwapchainImageOpenGLESKHR *) calloc(
            sizeof(XrSwapchainImageOpenGLESKHR), imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        swapchain_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }

    CHECK_XRCMD(xrEnumerateSwapchainImages(viewsurface.swapchain, imageCount, &imageCount,
                                           (XrSwapchainImageBaseHeader *) swapchain_images))


    for (uint32_t i = 0; i < imageCount; i++) {
        GLuint tex_c = swapchain_images[i].image;
        GLuint tex_z = 0;
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);

        GLint width;
        GLint height;
        glBindTexture(GL_TEXTURE_2D, tex_c);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);

        glGenTextures(1, &tex_z);
        glBindTexture(GL_TEXTURE_2D, tex_z);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT,
                     GL_UNSIGNED_INT, nullptr);

        render_target_t rtarget;
        rtarget.texc_id = tex_c;
        rtarget.texz_id = tex_z;
        rtarget.fbo_id = fbo;
        rtarget.width = viewsurface.width;
        rtarget.height = viewsurface.height;
        viewsurface.render_targets.push_back(rtarget);

        LOG_INFO("SwapchainImage[%d/%d] FBO:%d, TEXC:%d, TEXZ:%d, WH(%d, %d)", i + 1, imageCount,
                 fbo,
                 tex_c,
                 tex_z, viewsurface.width, viewsurface.height);
    }
    free(swapchain_images);
}

int openxr_acquire_viewsurface(viewsurface_t &viewSurface, render_target_t &renderTarget,
                               XrSwapchainSubImage &subImage) {
    subImage.swapchain = viewSurface.swapchain;
    subImage.imageRect.offset.x = 0;
    subImage.imageRect.offset.y = 0;
    subImage.imageRect.extent.width = viewSurface.width;
    subImage.imageRect.extent.height = viewSurface.height;
    subImage.imageArrayIndex = 0;

    uint32_t imageIndex = openxr_acquire_swapchain_img(viewSurface.swapchain);
    renderTarget = viewSurface.render_targets[imageIndex];

    return 0;
}

int openxr_release_viewsurface(viewsurface_t &viewSurface) {
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    CHECK_XRCMD(xrReleaseSwapchainImage(viewSurface.swapchain, &releaseInfo));

    return 0;
}

int openxr_acquire_swapchain_img(XrSwapchain swapchain) {
    uint32_t imgIdx;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    CHECK_XRCMD(xrAcquireSwapchainImage(swapchain, &acquireInfo, &imgIdx))

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    CHECK_XRCMD(xrWaitSwapchainImage(swapchain, &waitInfo))

    return imgIdx;
}

XrActionSet
openxr_create_actionset(XrInstance *instance, std::string name, std::string localized_name,
                        int priority) {
    XrActionSet actionset;
    XrActionSetCreateInfo ci = {XR_TYPE_ACTION_SET_CREATE_INFO};
    ci.priority = priority;
    strncpy(ci.actionSetName, name.c_str(), XR_MAX_ACTION_SET_NAME_SIZE - 1);
    ci.actionSetName[XR_MAX_ACTION_SET_NAME_SIZE - 1] = '\0';
    strncpy(ci.localizedActionSetName, localized_name.c_str(), XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    ci.localizedActionSetName[XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1] = '\0';
    CHECK_XRCMD(xrCreateActionSet(*instance, &ci, &actionset))

    return actionset;
}

XrAction openxr_create_action(XrActionSet *actionSet, XrActionType type, std::string name,
                              std::string localized_name, int subpath_number,
                              XrPath *subpath_array) {
    XrActionCreateInfo aci = {};
    aci.type = XR_TYPE_ACTION_CREATE_INFO;
    aci.next = nullptr;
    aci.actionType = type;
    aci.countSubactionPaths = subpath_number;
    aci.subactionPaths = subpath_array;

    strncpy(aci.actionName, name.c_str(), XR_MAX_ACTION_NAME_SIZE);
    strncpy(aci.localizedActionName, localized_name.c_str(), XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    XrAction action;
    CHECK_XRCMD(xrCreateAction(*actionSet, &aci, &action))
    return action;
}

XrPath openxr_string2path(XrInstance *instance, std::string str) {
    XrPath path;
    CHECK_XRCMD(xrStringToPath(*instance, str.c_str(), &path));
    return path;
}

int openxr_bind_interaction(XrInstance *instance, std::string profile,
                            std::vector<XrActionSuggestedBinding> &bindings) {
    XrPath profPath;
    CHECK_XRCMD(xrStringToPath(*instance, profile.c_str(), &profPath));

    XrInteractionProfileSuggestedBinding bind{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    bind.interactionProfile = profPath;
    bind.suggestedBindings = bindings.data();
    bind.countSuggestedBindings = (uint32_t) bindings.size();
    CHECK_XRCMD(xrSuggestInteractionProfileBindings(*instance, &bind));
    return 0;
}

int openxr_attach_actionset(XrSession *session, XrActionSet actionSet) {
    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet;
    CHECK_XRCMD(xrAttachSessionActionSets(*session, &attachInfo));

    return 0;
}

XrSpace openxr_create_action_space(XrSession *session, XrAction action, XrPath path) {

    XrActionSpaceCreateInfo actionSpaceInfo = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    actionSpaceInfo.action = action;
    actionSpaceInfo.poseInActionSpace.orientation.w = 1.0f;
    actionSpaceInfo.subactionPath = path;
    XrSpace space;
    CHECK_XRCMD(xrCreateActionSpace(*session, &actionSpaceInfo, &space))

    return space;
}

static XrEventDataBuffer s_evDataBuf;

static XrEventDataBaseHeader *
openxr_poll_event(XrInstance *instance, XrSession *session) {
    XrEventDataBaseHeader *ev = reinterpret_cast<XrEventDataBaseHeader *>(&s_evDataBuf);
    *ev = {XR_TYPE_EVENT_DATA_BUFFER};

    XrResult xr = xrPollEvent(*instance, &s_evDataBuf);
    if (xr == XR_EVENT_UNAVAILABLE)
        return nullptr;

    if (xr != XR_SUCCESS) {
        LOG_ERROR("xrPollEvent");
        return NULL;
    }

    if (ev->type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
        XrEventDataEventsLost *evLost = reinterpret_cast<XrEventDataEventsLost *>(ev);
        LOG_ERROR("%p events lost", evLost);
    }
    return ev;
}

static XrSessionState s_session_state = XR_SESSION_STATE_UNKNOWN;
static bool s_session_running = false;

void openxr_create_session(XrInstance *instance, XrSystemId *system_id, XrSession *session) {
    CHECK(*instance != XR_NULL_HANDLE)
    CHECK(*session == XR_NULL_HANDLE)

    XrGraphicsBindingOpenGLESAndroidKHR openxr_graphics_binding{
            XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};

    openxr_graphics_binding.display = egl_get_display();
    openxr_graphics_binding.config = egl_get_config();
    openxr_graphics_binding.context = egl_get_context();

    {
        LOG_INFO("Creating session...");
        XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
        createInfo.next = &openxr_graphics_binding;
        createInfo.systemId = *system_id;
        CHECK_XRCMD(xrCreateSession(*instance, &createInfo, session))
    }
}


int openxr_begin_session(XrSession *session) {
    XrViewConfigurationType viewType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
    bi.primaryViewConfigurationType = viewType;
    CHECK_XRCMD(xrBeginSession(*session, &bi))

    return 0;
}

int openxr_handle_session_state_changed(XrSession *session, XrEventDataSessionStateChanged &ev, bool *exitLoop, bool *reqRestart) {
    XrSessionState old_state = s_session_state;
    XrSessionState new_state = ev.state;
    s_session_state = new_state;

    LOG_INFO("  [SessionState]: %s -> %s (session=%p, time=%ld)",
             to_string(old_state), to_string(new_state), ev.session, ev.time);

    if ((ev.session != XR_NULL_HANDLE) &&
        (ev.session != *session)) {
        LOG_ERROR("XrEventDataSessionStateChanged for unknown session");
        return -1;
    }

    switch (new_state) {
        case XR_SESSION_STATE_READY:
            openxr_begin_session(session);
            s_session_running = true;
            break;

        case XR_SESSION_STATE_STOPPING:
            xrEndSession(*session);
            s_session_running = false;
            break;

        case XR_SESSION_STATE_EXITING:
            *exitLoop = true;
            *reqRestart = false;    // Do not attempt to restart because user closed this session.
            break;

        case XR_SESSION_STATE_LOSS_PENDING:
            *exitLoop = true;
            *reqRestart = true;     // Poll for a new instance.
            break;
        default:
            break;

    }
    return 0;
}

bool openxr_is_session_running() {
    return s_session_running;
}


int
openxr_poll_events(XrInstance *instance, XrSession *session, bool *exit, bool *request_restart, bool *mounted) {
    *exit = false;
    *request_restart = false;

    // Process all pending messages.
    while (XrEventDataBaseHeader *ev = openxr_poll_event(instance, session)) {
        switch (ev->type) {
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
                LOG_ERROR("XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING");
                *exit = true;
                *request_restart = true;
                return -1;
            }

            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                LOG_ERROR("XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED");
                XrEventDataSessionStateChanged sess_ev = *(XrEventDataSessionStateChanged *) ev;
                openxr_handle_session_state_changed(session, sess_ev, exit, request_restart);
                break;
            }

            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                LOG_ERROR ("XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED");
                break;

            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                LOG_ERROR("XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING");
                break;

            case XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT: {
                LOG_ERROR("XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT");
                XrEventDataUserPresenceChangedEXT presence_ev = *(XrEventDataUserPresenceChangedEXT *) ev;
                if (presence_ev.isUserPresent == XR_TRUE) {
                    LOG_INFO("User is present");
                    *mounted = true;
                } else {
                    LOG_INFO("User is not present");
                    *mounted = false;
                }
                break;
            }
            default:
                LOG_ERROR("Unknown event type %d", ev->type);
                break;
        }
    }
    return 0;
}

int openxr_begin_frame(XrSession *session, XrTime *display_time) {

    XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    CHECK_XRCMD(xrWaitFrame(*session, &frameWaitInfo, &frameState))

    XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    CHECK_XRCMD(xrBeginFrame(*session, &frameBeginInfo))
    *display_time = frameState.predictedDisplayTime;

    return (int) frameState.shouldRender;
}

int openxr_end_frame(XrSession *session, XrTime *displayTime,
                     std::vector<XrCompositionLayerBaseHeader *> &layers) {

    XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
    frameEndInfo.displayTime = *displayTime;
    frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    frameEndInfo.layerCount = (uint32_t) layers.size();
    frameEndInfo.layers = layers.data();
    CHECK_XRCMD(xrEndFrame(*session, &frameEndInfo))
    return 0;
}

int openxr_locate_views(XrSession *session, XrTime *displayTime, XrSpace space, uint32_t viewCount,
                        XrView *view_array) {

    XrViewState viewState{XR_TYPE_VIEW_STATE};

    XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    viewLocateInfo.displayTime = *displayTime;
    viewLocateInfo.space = space;

    auto viewCapacityInput = (uint32_t) viewCount;
    uint32_t viewCountOutput;

    auto res = CHECK_XRCMD(xrLocateViews(*session, &viewLocateInfo, &viewState, viewCapacityInput,
                                         &viewCountOutput, view_array));
    CHECK_XRRESULT(res, "xrLocateViews")
    if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
        return false;
    }

    CHECK(viewCountOutput == viewCapacityInput)
    CHECK(viewCountOutput == viewCount)

    return 0;
}

std::string openxr_get_runtime_name(XrInstance *instance) {
    XrInstanceProperties prop = {XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(*instance, &prop);

    char strbuf[128];
    snprintf(strbuf, 127, "%s (%u.%u.%u)", prop.runtimeName,
             XR_VERSION_MAJOR (prop.runtimeVersion),
             XR_VERSION_MINOR (prop.runtimeVersion),
             XR_VERSION_PATCH (prop.runtimeVersion));
    std::string runtime_name = strbuf;
    return runtime_name;
}

std::string openxr_get_system_name(XrInstance *instance, XrSystemId *system_id) {
    XrSystemProperties prop = {XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(*instance, *system_id, &prop);

    std::string sys_name = prop.systemName;
    return sys_name;
}

void openxr_has_user_presence_capability(XrInstance *instance, XrSystemId *system_id) {
    XrSystemUserPresencePropertiesEXT presenceProps = {XR_TYPE_SYSTEM_USER_PRESENCE_PROPERTIES_EXT};
    XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES, &presenceProps};
    CHECK_XRCMD(xrGetSystemProperties(*instance, *system_id, &sysProps))
    if (presenceProps.supportsUserPresence) {
        LOG_INFO("System supports user presence");
    } else {
        LOG_INFO("System doesn't support user presence");
    }
}