#include "vr.h"
#include "gfx.h"
#include <unknwn.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstring>

struct VR::Impl {
    XrInstance inst = XR_NULL_HANDLE;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrSwapchain chain[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<XrSwapchainImageOpenGLKHR> images[2];
    GLuint depth[2] = {0, 0};
    uint32_t imageIndex[2] = {0, 0};
    XrView views[2];
    XrTime displayTime = 0;
    bool render = false;
    Mat4 recenter = identity();
};

static Mat4 poseMat(const XrPosef& p) {
    return fromQuatPos(Quat{p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z}, {p.position.x, p.position.y, p.position.z});
}

bool VR::init() {
    d = new Impl;
    const char* exts[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "worldridr");
    strcpy(ci.applicationInfo.engineName, "worldridr");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    XrResult r = xrCreateInstance(&ci, &d->inst);
    if (XR_FAILED(r)) { logMsg("VR: keine OpenXR-Laufzeit gefunden (Meta Horizon Link gestartet und als OpenXR-Laufzeit gesetzt?) [%d]", (int)r); return false; }
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(d->inst, &si, &d->sys);
    if (XR_FAILED(r)) { logMsg("VR: kein Headset verbunden [%d]", (int)r); return false; }
    PFN_xrGetOpenGLGraphicsRequirementsKHR getReq = nullptr;
    xrGetInstanceProcAddr(d->inst, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq);
    XrGraphicsRequirementsOpenGLKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    if (getReq) getReq(d->inst, d->sys, &req);
    XrGraphicsBindingOpenGLWin32KHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    gb.hDC = wglGetCurrentDC();
    gb.hGLRC = wglGetCurrentContext();
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = d->sys;
    r = xrCreateSession(d->inst, &sci, &d->session);
    if (XR_FAILED(r)) { logMsg("VR: Sitzung konnte nicht erstellt werden [%d]", (int)r); return false; }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1;
    xrCreateReferenceSpace(d->session, &rs, &d->space);

    uint32_t n = 0;
    XrViewConfigurationView cfg[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    r = xrEnumerateViewConfigurationViews(d->inst, d->sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, cfg);
    if (XR_FAILED(r) || n != 2) { logMsg("VR: Stereo-Ansicht nicht verfuegbar"); return false; }
    std::vector<int64_t> formats;
    xrEnumerateSwapchainFormats(d->session, 0, &n, nullptr);
    formats.resize(n);
    xrEnumerateSwapchainFormats(d->session, n, &n, formats.data());
    int64_t format = formats.empty() ? GL_RGBA8 : formats[0];
    for (int64_t want : {(int64_t)GL_SRGB8_ALPHA8, (int64_t)GL_RGBA8}) {
        bool hit = false;
        for (int64_t f : formats) if (f == want) { format = want; hit = true; break; }
        if (hit) break;
    }
    for (int i = 0; i < 2; i++) {
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = format;
        sc.sampleCount = 1;
        sc.width = cfg[i].recommendedImageRectWidth;
        sc.height = cfg[i].recommendedImageRectHeight;
        sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        r = xrCreateSwapchain(d->session, &sc, &d->chain[i]);
        if (XR_FAILED(r)) { logMsg("VR: Swapchain fehlgeschlagen [%d]", (int)r); return false; }
        xrEnumerateSwapchainImages(d->chain[i], 0, &n, nullptr);
        d->images[i].assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        xrEnumerateSwapchainImages(d->chain[i], n, &n, (XrSwapchainImageBaseHeader*)d->images[i].data());
        eye[i].w = (int)sc.width; eye[i].h = (int)sc.height;
        glGenFramebuffers(1, &eye[i].fbo);
        glGenRenderbuffers(1, &d->depth[i]);
        glBindRenderbuffer(GL_RENDERBUFFER, d->depth[i]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, eye[i].w, eye[i].h);
        d->views[i] = {XR_TYPE_VIEW};
    }
    available = true;
    logMsg("VR bereit: %dx%d pro Auge", eye[0].w, eye[0].h);
    return true;
}

void VR::poll() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(d->inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = (XrEventDataSessionStateChanged*)&ev;
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running = XR_SUCCEEDED(xrBeginSession(d->session, &bi));
                wantRecenter = true;
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(d->session);
                running = false;
            } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                running = false;
                available = false;
            }
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool VR::shouldRender() const { return d && d->render; }

bool VR::beginFrame() {
    if (!available) return false;
    poll();
    if (!running) return false;
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(d->session, nullptr, &fs))) return false;
    if (XR_FAILED(xrBeginFrame(d->session, nullptr))) return false;
    d->displayTime = fs.predictedDisplayTime;
    d->render = fs.shouldRender == XR_TRUE;
    if (d->render) {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = d->displayTime;
        li.space = d->space;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t n = 0;
        if (XR_FAILED(xrLocateViews(d->session, &li, &vs, 2, &n, d->views)) || n != 2 ||
            !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) || !(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
            d->render = false;
    }
    if (d->render) {
        Mat4 a = poseMat(d->views[0].pose), b = poseMat(d->views[1].pose);
        if (wantRecenter) {
            // Kopfposition und Blickrichtung (nur Drehung um die Hochachse) als neuen Sitz-Nullpunkt setzen
            Vec3 c{(a.m[12] + b.m[12]) * 0.5f, (a.m[13] + b.m[13]) * 0.5f, (a.m[14] + b.m[14]) * 0.5f};
            Vec3 f = xformDir(a, {0, 0, -1});
            float yaw = std::atan2(-f.x, -f.z);
            d->recenter = rigidInverse(fromQuatPos(qaxis({0, 1, 0}, yaw), c));
            wantRecenter = false;
        }
        for (int i = 0; i < 2; i++) {
            eye[i].pose = mul(d->recenter, i == 0 ? a : b);
            const XrFovf& fv = d->views[i].fov;
            eye[i].proj = frustumTan(std::tan(fv.angleLeft), std::tan(fv.angleRight), std::tan(fv.angleDown), std::tan(fv.angleUp), 0.08f, 40000.0f);
        }
    }
    return true;
}

void VR::bindEye(int i) {
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(d->chain[i], &ai, &d->imageIndex[i]);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(d->chain[i], &wi);
    glBindFramebuffer(GL_FRAMEBUFFER, eye[i].fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->images[i][d->imageIndex[i]].image, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, d->depth[i]);
    glViewport(0, 0, eye[i].w, eye[i].h);
}

void VR::releaseEye(int i) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(d->chain[i], &ri);
}

void VR::endFrame() {
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1] = {(const XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = d->displayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (d->render) {
        for (int i = 0; i < 2; i++) {
            pv[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            pv[i].pose = d->views[i].pose;
            pv[i].fov = d->views[i].fov;
            pv[i].subImage.swapchain = d->chain[i];
            pv[i].subImage.imageRect.offset = {0, 0};
            pv[i].subImage.imageRect.extent = {eye[i].w, eye[i].h};
        }
        layer.space = d->space;
        layer.viewCount = 2;
        layer.views = pv;
        ei.layerCount = 1;
        ei.layers = layers;
    }
    xrEndFrame(d->session, &ei);
}
