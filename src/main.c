#include <stdio.h>

#define WIN_32_LEAN_AND_MEAN
#include <windows.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

#include "glad/glad.h"
#include "mathc.h"
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"

// Capacities / Constants
#define MAX_VIEWS 4
#define MAX_FORMATS 32
#define MAX_SWAPCHAIN_IMAGES 8

#define HAND_LEFT_INDEX 0
#define HAND_RIGHT_INDEX 1
#define HAND_COUNT 2

static void mat4_proj_xr(float result[16], XrFovf fov, float near_z, float far_z)
{
    const float tan_left = tanf(fov.angleLeft);
    const float tan_right = tanf(fov.angleRight);

    const float tan_down = tanf(fov.angleDown);
    const float tan_up = tanf(fov.angleUp);

    const float tan_width = tan_right - tan_left;
    const float tan_height = (tan_up - tan_down);

    const float offset_z = near_z;

    result[0] = 2 / tan_width;
    result[4] = 0;
    result[8] = (tan_right + tan_left) / tan_width;
    result[12] = 0;

    result[1] = 0;
    result[5] = 2 / tan_height;
    result[9] = (tan_up + tan_down) / tan_height;
    result[13] = 0;

    result[2] = 0;
    result[6] = 0;
    result[10] = -(far_z + offset_z) / (far_z - near_z);
    result[14] = -(far_z * (near_z + offset_z)) / (far_z - near_z);

    result[3] = 0;
    result[7] = 0;
    result[11] = -1;
    result[15] = 0;
}

// Static application state
typedef struct state_t
{
    SDL_Window *desktop_window;
    SDL_GLContext *gl_context;

    float near_z;
    float far_z;

    XrInstance instance;
    XrSystemId system_id;
    XrSystemProperties system_props;
    XrGraphicsRequirementsOpenGLKHR opengl_reqs;
    XrSession session;
    XrSpace play_space;

    uint32_t view_count;
    XrViewConfigurationView view_confs[MAX_VIEWS];
    XrView views[MAX_VIEWS];
    XrCompositionLayerProjectionView proj_views[MAX_VIEWS];

    uint32_t swapchain_count;
    XrSwapchain swapchains[MAX_VIEWS];
    uint32_t swapchain_lengths[MAX_VIEWS];
    XrSwapchainImageOpenGLKHR swapchain_images[MAX_VIEWS][MAX_SWAPCHAIN_IMAGES];
    GLuint framebuffers[MAX_VIEWS][MAX_SWAPCHAIN_IMAGES];

    uint32_t depth_count;
    XrSwapchain depths[MAX_VIEWS];
    uint32_t depth_lengths[MAX_VIEWS];
    XrCompositionLayerDepthInfoKHR depth_infos[MAX_VIEWS];
    XrSwapchainImageOpenGLKHR depth_images[MAX_VIEWS][MAX_SWAPCHAIN_IMAGES];

    XrPath hand_paths[HAND_COUNT];
    XrPath select_click_path[HAND_COUNT];
    XrPath trigger_value_path[HAND_COUNT];
    XrPath thumbstick_y_path[HAND_COUNT];
    XrPath grip_pose_path[HAND_COUNT];
    XrPath haptic_path[HAND_COUNT];

    XrActionSet gameplay_actionset;
    XrAction hand_pose_action;
    XrAction grab_action_float;
    XrAction haptic_action;

    XrSpace hand_pose_spaces[HAND_COUNT];

    GLuint shader;
    GLuint vao;
} state_t;
static state_t state;

static void render_block(float position[3], float orientation[4], float radii[3], int modelLoc)
{
    float model[16];
    float scale[16];
    float rotation[16];
    float translation[16];

    mat4_identity(translation);
    mat4_translation(translation, translation, position);
    mat4_rotation_quat(rotation, orientation);
    mat4_identity(scale);
    mat4_scaling(scale, scale, radii);
    mat4_multiply(model, rotation, scale);
    mat4_multiply(model, translation, model);

    glUniformMatrix4fv(modelLoc, 1, GL_FALSE, model);
    glDrawArrays(GL_TRIANGLES, 0, 36);
}

void render_rotated_cube(float position[3], float cube_size, float rot, float projection_matrix[16], int modelLoc)
{
    float scale[16];
    float rotation[16];
    float translation[16];
    mat4_identity(scale);
    mat4_scaling(scale, scale, (float[3]){cube_size / 2.0f, cube_size / 2.0f, cube_size / 2.0f});
    mat4_identity(translation);
    mat4_translation(translation, translation, position);
    mat4_rotation_y(rotation, to_radians(rot));

    float model[16];
    mat4_multiply(model, scale, rotation);
    mat4_multiply(model, translation, model);

    glUniformMatrix4fv(modelLoc, 1, GL_FALSE, model);
    glDrawArrays(GL_TRIANGLES, 0, 36);
}

void render_frame(int w, int h, XrTime predictedDisplayTime, int view_index, XrSpaceLocation *hand_locations, float proj[16], float view[16], GLuint framebuffer, GLuint image, GLuint depthbuffer)
{
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    glViewport(0, 0, w, h);
    glScissor(0, 0, w, h);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthbuffer, 0);

    glClearColor(0.2f, 0.0f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(state.shader);
    glBindVertexArray(state.vao);

    int modelLoc = glGetUniformLocation(state.shader, "model");
    int colorLoc = glGetUniformLocation(state.shader, "uniformColor");
    int viewLoc = glGetUniformLocation(state.shader, "view");
    glUniformMatrix4fv(viewLoc, 1, GL_FALSE, view);
    int projLoc = glGetUniformLocation(state.shader, "proj");
    glUniformMatrix4fv(projLoc, 1, GL_FALSE, proj);

    {
        // the special color value (0, 0, 0) will get replaced by some UV color in the shader
        glUniform3f(colorLoc, 0.0, 0.0, 0.0);

        double display_time_seconds = ((double)predictedDisplayTime) / (1000. * 1000. * 1000.);
        const float rotations_per_sec = .25;
        float angle = ((long)(display_time_seconds * 360. * rotations_per_sec)) % 360;

        float dist = 1.5f;
        float height = 0.5f;
        render_rotated_cube((float[3]){0, height, -dist}, 0.33f, angle, proj, modelLoc);
        render_rotated_cube((float[3]){0, height, dist}, 0.33f, angle, proj, modelLoc);
        render_rotated_cube((float[3]){dist, height, 0}, 0.33f, angle, proj, modelLoc);
        render_rotated_cube((float[3]){-dist, height, 0}, 0.33f, angle, proj, modelLoc);
    }

    // render controllers
    for (int hand = 0; hand < 2; hand++)
    {
        if (hand == 0)
        {
            glUniform3f(colorLoc, 1.0, 0.5, 0.5);
        }
        else
        {
            glUniform3f(colorLoc, 0.5, 1.0, 0.5);
        }

        bool hand_location_valid =
            //(spaceLocation[hand].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
            (hand_locations[hand].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;

        // draw a block at the controller pose
        if (!hand_location_valid)
            continue;

        float scale[3] = {.05f, .05f, .2f};
        render_block((float *)&hand_locations[hand].pose.position, (float *)&hand_locations[hand].pose.orientation, scale, modelLoc);
    }

    // blit left eye to desktop window
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (view_index == 0)
    {
        glBlitNamedFramebuffer((GLuint)framebuffer,             // readFramebuffer
                               (GLuint)0,                       // backbuffer     // drawFramebuffer
                               (GLint)0,                        // srcX0
                               (GLint)0,                        // srcY0
                               (GLint)w,                        // srcX1
                               (GLint)h,                        // srcY1
                               (GLint)0,                        // dstX0
                               (GLint)0,                        // dstY0
                               (GLint)w / 2,                    // dstX1
                               (GLint)h / 2,                    // dstY1
                               (GLbitfield)GL_COLOR_BUFFER_BIT, // mask
                               (GLenum)GL_LINEAR);              // filter

        SDL_GL_SwapWindow(state.desktop_window);
    }
}

#undef main
int main()
{
    // Create Instance
    XrInstanceCreateInfo instance_create_info = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {
            .apiVersion = XR_CURRENT_API_VERSION,
            .applicationName = "Test App",
            .applicationVersion = 1,
            .engineName = "XR",
            .engineVersion = 1,
        },
        .enabledExtensionCount = 1,
        .enabledExtensionNames = (const char *[1]){XR_KHR_OPENGL_ENABLE_EXTENSION_NAME}};

    XrResult result = xrCreateInstance(&instance_create_info, &state.instance);
    if (result != XR_SUCCESS)
    {
        printf("Instance Creation Failed\n");
        return 1;
    }

    // Load extension function pointers
    static PFN_xrGetOpenGLGraphicsRequirementsKHR pfnGetOpenGLGraphicsRequirementsKHR = NULL;
    result = xrGetInstanceProcAddr(state.instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&pfnGetOpenGLGraphicsRequirementsKHR);
    if (result != XR_SUCCESS)
    {
        printf("Extension Load Failed\n");
        return 1;
    }

    // Get system
    XrSystemGetInfo system_get_info = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };

    result = xrGetSystem(state.instance, &system_get_info, &state.system_id);
    if (result != XR_SUCCESS)
    {
        printf("Failed to get System\n");
        return 1;
    }

    // Get system properties
    result = xrGetSystemProperties(state.instance, state.system_id, &state.system_props);
    if (result != XR_SUCCESS)
    {
        printf("Failed to get system properties\n");
        return 1;
    }

    printf("System properties for system %llu: \"%s\", vendor ID %d\n", state.system_props.systemId, state.system_props.systemName, state.system_props.vendorId);
    printf("\tMax layers          : %d\n", state.system_props.graphicsProperties.maxLayerCount);
    printf("\tMax swapchain height: %d\n", state.system_props.graphicsProperties.maxSwapchainImageHeight);
    printf("\tMax swapchain width : %d\n", state.system_props.graphicsProperties.maxSwapchainImageWidth);
    printf("\tOrientation Tracking: %d\n", state.system_props.trackingProperties.orientationTracking);
    printf("\tPosition Tracking   : %d\n", state.system_props.trackingProperties.positionTracking);

    // Get view configurations
    result = xrEnumerateViewConfigurationViews(state.instance, state.system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &state.view_count, NULL);
    if (result != XR_SUCCESS)
    {
        printf("Failed to get view count\n");
    }

    result = xrEnumerateViewConfigurationViews(state.instance, state.system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, MAX_VIEWS, &state.view_count, state.view_confs);
    if (result != XR_SUCCESS)
    {
        printf("Failed to get views\n");
    }

    for (uint32_t i = 0; i < state.view_count; i++)
    {
        printf("View Configuration View %d:\n", i);
        printf("\tResolution       : Recommended %dx%d, Max: %dx%d\n", state.view_confs[i].recommendedImageRectWidth, state.view_confs[i].recommendedImageRectHeight, state.view_confs[i].maxImageRectWidth, state.view_confs[i].maxImageRectHeight);
        printf("\tSwapchain Samples: Recommended: %d, Max: %d)\n", state.view_confs[i].recommendedSwapchainSampleCount, state.view_confs[i].maxSwapchainSampleCount);
    }

    // Check graphics requirements
    result = pfnGetOpenGLGraphicsRequirementsKHR(state.instance, state.system_id, &state.opengl_reqs);
    if (result != XR_SUCCESS)
    {
        printf("Failed to get graphics reqs\n");
        return 1;
    }

    printf("Supports OpenGL versions %llu to %llu\n", state.opengl_reqs.minApiVersionSupported, state.opengl_reqs.maxApiVersionSupported);

    // Init SDL and OpenGL
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        printf("Unable to initialize SDL\n");
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);

    /* Create our window centered at half the VR resolution */
    int w = state.view_confs[0].recommendedImageRectWidth;
    int h = state.view_confs[0].recommendedImageRectHeight;
    state.desktop_window = SDL_CreateWindow("OpenXR Example", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w / 2, h / 2, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!state.desktop_window)
    {
        printf("Unable to create window\n");
        return 1;
    }

    state.gl_context = SDL_GL_CreateContext(state.desktop_window);
    gladLoadGLLoader(SDL_GL_GetProcAddress);
    SDL_GL_SetSwapInterval(0);

    // Create Session
    XrGraphicsBindingOpenGLWin32KHR graphics_binding_gl = {
        .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR,
        .hDC = wglGetCurrentDC(),
        .hGLRC = wglGetCurrentContext(),
    };

    XrSessionCreateInfo session_create_info = {
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = &graphics_binding_gl,
        .systemId = state.system_id,
    };

    result = xrCreateSession(state.instance, &session_create_info, &state.session);
    if (result != XR_SUCCESS)
    {
        printf("Failed to create session\n");
        return 1;
    }

    // Create Play Space
    XrReferenceSpaceCreateInfo play_space_create_info = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .next = NULL,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE,
        .poseInReferenceSpace = {
            .orientation = {.x = 0, .y = 0, .z = 0, .w = 1.0},
            .position = {.x = 0, .y = 0, .z = 0},
        },
    };

    result = xrCreateReferenceSpace(state.session, &play_space_create_info, &state.play_space);
    if (result != XR_SUCCESS)
    {
        printf("Failed to create play space\n");
        return 1;
    }

    // Begin Session
    // XrSessionBeginInfo session_begin_info = {
    //     .type = XR_TYPE_SESSION_BEGIN_INFO,
    //     .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
    // };

    // result = xrBeginSession(state.session, &session_begin_info);
    // if (result != XR_SUCCESS)
    // {
    //     printf("Failed to begin session\n");
    //     return 1;
    // }

    // Create Swapchains
    uint32_t swapchain_format_count;
    result = xrEnumerateSwapchainFormats(state.session, 0, &swapchain_format_count, NULL);
    if (result != XR_SUCCESS)
    {
        printf("Failed to get swapchain format count\n");
        return 1;
    }

    int64_t swapchain_formats[MAX_FORMATS];
    result = xrEnumerateSwapchainFormats(state.session, swapchain_format_count, &swapchain_format_count, swapchain_formats);
    if (result != XR_SUCCESS)
    {
        printf("Failed to enumerate swapchain formats\n");
        return 1;
    }

    int64_t color_format = swapchain_formats[0];
    int64_t depth_format = -1;
    for (int i = 0; i < swapchain_format_count; i++)
    {
        if (swapchain_formats[i] == GL_SRGB8_ALPHA8)
            color_format = GL_SRGB8_ALPHA8;

        if (swapchain_formats[i] == GL_DEPTH_COMPONENT16)
            depth_format = GL_DEPTH_COMPONENT16;
    }

    state.swapchain_count = state.view_count;
    for (int i = 0; i < state.swapchain_count; i++)
    {
        // Color Swapchain
        XrSwapchainCreateInfo swapchain_create_info = {
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
            .format = color_format,
            .sampleCount = state.view_confs[i].recommendedSwapchainSampleCount,
            .width = state.view_confs[i].recommendedImageRectWidth,
            .height = state.view_confs[i].recommendedImageRectHeight,
            .faceCount = 1,
            .arraySize = 1,
            .mipCount = 1,
        };

        result = xrCreateSwapchain(state.session, &swapchain_create_info, &state.swapchains[i]);
        if (result != XR_SUCCESS)
        {
            printf("Failed to create swapchain\n");
            return 1;
        }

        result = xrEnumerateSwapchainImages(state.swapchains[i], 0, &state.swapchain_lengths[i], NULL);
        if (result != XR_SUCCESS)
        {
            printf("Failed to enumerate swapchain images\n");
            return 1;
        }

        for (uint32_t j = 0; j < state.swapchain_lengths[i]; j++)
        {
            state.swapchain_images[i][j].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        }

        result = xrEnumerateSwapchainImages(state.swapchains[i], state.swapchain_lengths[i], &state.swapchain_lengths[i], (XrSwapchainImageBaseHeader *)state.swapchain_images[i]);
        if (result != XR_SUCCESS)
        {
            printf("Failed to enumerate swapchain images\n");
            return 1;
        }
    }

    state.depth_count = state.view_count;
    for (int i = 0; i < state.swapchain_count; i++)
    {
        // Depth Swapchain
        XrSwapchainCreateInfo swapchain_create_info = {
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .format = depth_format,
            .sampleCount = state.view_confs[i].recommendedSwapchainSampleCount,
            .width = state.view_confs[i].recommendedImageRectWidth,
            .height = state.view_confs[i].recommendedImageRectHeight,
            .faceCount = 1,
            .arraySize = 1,
            .mipCount = 1,
        };

        result = xrCreateSwapchain(state.session, &swapchain_create_info, &state.depths[i]);
        if (result != XR_SUCCESS)
        {
            printf("Failed to create swapchain\n");
            return 1;
        }

        result = xrEnumerateSwapchainImages(state.depths[i], 0, &state.depth_lengths[i], NULL);
        if (result != XR_SUCCESS)
        {
            printf("Failed to enumerate swapchain images\n");
            return 1;
        }

        // these are wrappers for the actual OpenGL texture id
        for (uint32_t j = 0; j < state.depth_lengths[i]; j++)
        {
            state.depth_images[i][j].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        }

        result = xrEnumerateSwapchainImages(state.depths[i], state.depth_lengths[i], &state.depth_lengths[i], (XrSwapchainImageBaseHeader *)state.depth_images[i]);
        if (result != XR_SUCCESS)
        {
            printf("Failed to enumerate swapchain images\n");
            return 1;
        }
    }

    // Create views, projection views, depth infos
    for (int i = 0; i < state.view_count; i++)
    {
        state.views[i].type = XR_TYPE_VIEW;
    }

    state.near_z = 0.01f;
    state.far_z = 100.0f;

    for (int i = 0; i < state.view_count; i++)
    {
        state.proj_views[i] = (XrCompositionLayerProjectionView){
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
            .subImage = {
                .swapchain = state.swapchains[i],
                .imageRect = {
                    .extent.width = state.view_confs[i].recommendedImageRectWidth,
                    .extent.height = state.view_confs[i].recommendedImageRectHeight,
                },
            },
        };
    };

    for (uint32_t i = 0; i < state.view_count; i++)
    {
        state.depth_infos[i] = (XrCompositionLayerDepthInfoKHR){
            .type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
            .nearZ = state.near_z,
            .farZ = state.far_z,
            .subImage = {
                .swapchain = state.depths[i],
                .imageRect = {
                    .offset.x = 0,
                    .offset.y = 0,
                    .extent.width = state.view_confs[i].recommendedImageRectWidth,
                    .extent.height = state.view_confs[i].recommendedImageRectHeight,
                },
            },

        };

        state.proj_views[i].next = &state.depth_infos[i];
    };

    // Setup Inputs/Actions/Poses
    xrStringToPath(state.instance, "/user/hand/left", &state.hand_paths[HAND_LEFT_INDEX]);
    xrStringToPath(state.instance, "/user/hand/right", &state.hand_paths[HAND_RIGHT_INDEX]);
    xrStringToPath(state.instance, "/user/hand/left/input/select/click", &state.select_click_path[HAND_LEFT_INDEX]);
    xrStringToPath(state.instance, "/user/hand/right/input/select/click", &state.select_click_path[HAND_RIGHT_INDEX]);
    xrStringToPath(state.instance, "/user/hand/left/input/trigger/value", &state.trigger_value_path[HAND_LEFT_INDEX]);
    xrStringToPath(state.instance, "/user/hand/right/input/trigger/value", &state.trigger_value_path[HAND_RIGHT_INDEX]);
    xrStringToPath(state.instance, "/user/hand/left/input/thumbstick/y", &state.thumbstick_y_path[HAND_LEFT_INDEX]);
    xrStringToPath(state.instance, "/user/hand/right/input/thumbstick/y", &state.thumbstick_y_path[HAND_RIGHT_INDEX]);
    xrStringToPath(state.instance, "/user/hand/left/input/grip/pose", &state.grip_pose_path[HAND_LEFT_INDEX]);
    xrStringToPath(state.instance, "/user/hand/right/input/grip/pose", &state.grip_pose_path[HAND_RIGHT_INDEX]);
    xrStringToPath(state.instance, "/user/hand/left/output/haptic", &state.haptic_path[HAND_LEFT_INDEX]);
    xrStringToPath(state.instance, "/user/hand/right/output/haptic", &state.haptic_path[HAND_RIGHT_INDEX]);

    XrActionSetCreateInfo gameplay_actionset_info = {
        .type = XR_TYPE_ACTION_SET_CREATE_INFO,
        .actionSetName = "gampeplay_actionset",
        .localizedActionSetName = "Gameplay Actions",
    };

    result = xrCreateActionSet(state.instance, &gameplay_actionset_info, &state.gameplay_actionset);
    if (result != XR_SUCCESS)
    {
        printf("Failed to create actionset\n");
        return 1;
    }

    XrActionCreateInfo hand_action_info = {
        .type = XR_TYPE_ACTION_CREATE_INFO,
        .actionType = XR_ACTION_TYPE_POSE_INPUT,
        .actionName = "handpose",
        .localizedActionName = "Hand Pose",
        .countSubactionPaths = HAND_COUNT,
        .subactionPaths = state.hand_paths,
    };

    result = xrCreateAction(state.gameplay_actionset, &hand_action_info, &state.hand_pose_action);
    if (result != XR_SUCCESS)
    {
        printf("Failed to create hand pose action\n");
        return 1;
    }

    for (int i = 0; i < HAND_COUNT; i++)
    {
        XrActionSpaceCreateInfo action_space_info = {
            .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
            .action = state.hand_pose_action,
            .poseInActionSpace = {
                .orientation = {.x = 0, .y = 0, .z = 0, .w = 1.0},
                .position = {.x = 0, .y = 0, .z = 0},
            },
            .subactionPath = state.hand_paths[i],
        };

        result = xrCreateActionSpace(state.session, &action_space_info, &state.hand_pose_spaces[i]);
        if (result != XR_SUCCESS)
        {
            printf("Failed to create hand action space\n");
            return 1;
        }
    }

    // Grabbing objects is not actually implemented in this demo, it only gives some  haptic feebdack.
    XrActionCreateInfo grab_action_info = {
        .type = XR_TYPE_ACTION_CREATE_INFO,
        .actionType = XR_ACTION_TYPE_FLOAT_INPUT,
        .actionName = "grabobjectfloat",
        .localizedActionName = "Grab Object",
        .countSubactionPaths = HAND_COUNT,
        .subactionPaths = state.hand_paths,
    };

    result = xrCreateAction(state.gameplay_actionset, &grab_action_info, &state.grab_action_float);
    if (result != XR_SUCCESS)
    {
        printf("Failed to create grab action\n");
        return 1;
    }

    XrActionCreateInfo haptic_action_info = {
        .type = XR_TYPE_ACTION_CREATE_INFO,
        .next = NULL,
        .actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT,
        .actionName = "haptic",
        .localizedActionName = "Haptic Vibration",
        .countSubactionPaths = HAND_COUNT,
        .subactionPaths = state.hand_paths,
    };

    result = xrCreateAction(state.gameplay_actionset, &haptic_action_info, &state.haptic_action);
    if (result != XR_SUCCESS)
    {
        printf("Failed to create haptic action\n");
        return 1;
    }

    // Suggest Simple Controller Actions
    {
        XrPath interaction_profile_path;
        result = xrStringToPath(state.instance, "/interaction_profiles/khr/simple_controller", &interaction_profile_path);
        if (result != XR_SUCCESS)
        {
            printf("Failed to create simple controller path\n");
            return 1;
        }

        const XrActionSuggestedBinding bindings[] = {
            {.action = state.hand_pose_action, .binding = state.grip_pose_path[HAND_LEFT_INDEX]},
            {.action = state.hand_pose_action, .binding = state.grip_pose_path[HAND_RIGHT_INDEX]},
            // boolean input select/click will be converted to float that is either 0 or 1
            {.action = state.grab_action_float, .binding = state.select_click_path[HAND_LEFT_INDEX]},
            {.action = state.grab_action_float, .binding = state.select_click_path[HAND_RIGHT_INDEX]},
            {.action = state.haptic_action, .binding = state.haptic_path[HAND_LEFT_INDEX]},
            {.action = state.haptic_action, .binding = state.haptic_path[HAND_RIGHT_INDEX]},
        };

        const XrInteractionProfileSuggestedBinding suggested_bindings = {
            .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
            .interactionProfile = interaction_profile_path,
            .countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]),
            .suggestedBindings = bindings,
        };

        result = xrSuggestInteractionProfileBindings(state.instance, &suggested_bindings);
        if (result != XR_SUCCESS)
        {
            printf("Failed to suggest simple controller bindings\n");
            return 1;
        }
    }

    // Suggest Valve Index Actions
    {
        XrPath interaction_profile_path;
        result = xrStringToPath(state.instance, "/interaction_profiles/valve/index_controller", &interaction_profile_path);
        if (result != XR_SUCCESS)
        {
            printf("Failed to create index controller path\n");
            return 1;
        }

        const XrActionSuggestedBinding bindings[] = {
            {.action = state.hand_pose_action, .binding = state.grip_pose_path[HAND_LEFT_INDEX]},
            {.action = state.hand_pose_action, .binding = state.grip_pose_path[HAND_RIGHT_INDEX]},
            {.action = state.grab_action_float, .binding = state.trigger_value_path[HAND_LEFT_INDEX]},
            {.action = state.grab_action_float, .binding = state.trigger_value_path[HAND_RIGHT_INDEX]},
            {.action = state.haptic_action, .binding = state.haptic_path[HAND_LEFT_INDEX]},
            {.action = state.haptic_action, .binding = state.haptic_path[HAND_RIGHT_INDEX]},
        };

        const XrInteractionProfileSuggestedBinding suggested_bindings = {
            .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
            .interactionProfile = interaction_profile_path,
            .countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]),
            .suggestedBindings = bindings,
        };

        result = xrSuggestInteractionProfileBindings(state.instance, &suggested_bindings);
        if (result != XR_SUCCESS)
        {
            printf("Failed to suggest index controller bindings\n");
            return 1;
        }
    }

    // Setup up OpenGL state
    static const char *vert_src =
        "#version 330 core\n"
        "#extension GL_ARB_explicit_uniform_location : require\n"
        "layout(location = 0) in vec3 aPos;\n"
        "layout(location = 2) uniform mat4 model;\n"
        "layout(location = 3) uniform mat4 view;\n"
        "layout(location = 4) uniform mat4 proj;\n"
        "layout(location = 5) in vec2 aColor;\n"
        "out vec2 vertexColor;\n"
        "void main() {\n"
        "	gl_Position = proj * view * model * vec4(aPos.x, aPos.y, aPos.z, "
        "1.0);\n"
        "	vertexColor = aColor;\n"
        "}\n";

    static const char *frag_src =
        "#version 330 core\n"
        "#extension GL_ARB_explicit_uniform_location : require\n"
        "layout(location = 0) out vec4 FragColor;\n"
        "layout(location = 1) uniform vec3 uniformColor;\n"
        "in vec2 vertexColor;\n"
        "void main() {\n"
        "	FragColor = (uniformColor.x < 0.01 && uniformColor.y < 0.01 && "
        "uniformColor.z < 0.01) ? vec4(vertexColor, 1.0, 1.0) : vec4(uniformColor, "
        "1.0);\n"
        "}\n";

    for (int i = 0; i < state.view_count; i++)
    {
        glGenFramebuffers(state.swapchain_lengths[i], state.framebuffers[i]);
    }

    GLuint vert_shd = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vert_shd, 1, &vert_src, NULL);
    glCompileShader(vert_shd);
    int vertex_compile_res;
    glGetShaderiv(vert_shd, GL_COMPILE_STATUS, &vertex_compile_res);
    if (!vertex_compile_res)
    {
        char info_log[512];
        glGetShaderInfoLog(vert_shd, 512, NULL, info_log);
        printf("Vertex Shader failed to compile: %s\n", info_log);
        return 1;
    }

    GLuint frag_shd = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(frag_shd, 1, &frag_src, NULL);
    glCompileShader(frag_shd);
    int fragment_compile_res;
    glGetShaderiv(frag_shd, GL_COMPILE_STATUS, &fragment_compile_res);
    if (!fragment_compile_res)
    {
        char info_log[512];
        glGetShaderInfoLog(frag_shd, 512, NULL, info_log);
        printf("Fragment Shader failed to compile: %s\n", info_log);
        return 1;
    }

    state.shader = glCreateProgram();
    glAttachShader(state.shader, vert_shd);
    glAttachShader(state.shader, frag_shd);
    glLinkProgram(state.shader);
    GLint shader_program_res;
    glGetProgramiv(state.shader, GL_LINK_STATUS, &shader_program_res);
    if (!shader_program_res)
    {
        char info_log[512];
        glGetProgramInfoLog(state.shader, 512, NULL, info_log);
        printf("Shader Program failed to link: %s\n", info_log);
        return 1;
    }

    glDeleteShader(vert_shd);
    glDeleteShader(frag_shd);

    const float vertices[] = {
        -0.5f, -0.5f, -0.5f, 0.0f, 0.0f, 0.5f, -0.5f, -0.5f, 1.0f, 0.0f,
        0.5f, 0.5f, -0.5f, 1.0f, 1.0f, 0.5f, 0.5f, -0.5f, 1.0f, 1.0f,
        -0.5f, 0.5f, -0.5f, 0.0f, 1.0f, -0.5f, -0.5f, -0.5f, 0.0f, 0.0f,

        -0.5f, -0.5f, 0.5f, 0.0f, 0.0f, 0.5f, -0.5f, 0.5f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.5f, 1.0f, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f, 1.0f,
        -0.5f, 0.5f, 0.5f, 0.0f, 1.0f, -0.5f, -0.5f, 0.5f, 0.0f, 0.0f,

        -0.5f, 0.5f, 0.5f, 1.0f, 0.0f, -0.5f, 0.5f, -0.5f, 1.0f, 1.0f,
        -0.5f, -0.5f, -0.5f, 0.0f, 1.0f, -0.5f, -0.5f, -0.5f, 0.0f, 1.0f,
        -0.5f, -0.5f, 0.5f, 0.0f, 0.0f, -0.5f, 0.5f, 0.5f, 1.0f, 0.0f,

        0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.5f, 0.5f, -0.5f, 1.0f, 1.0f,
        0.5f, -0.5f, -0.5f, 0.0f, 1.0f, 0.5f, -0.5f, -0.5f, 0.0f, 1.0f,
        0.5f, -0.5f, 0.5f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f,

        -0.5f, -0.5f, -0.5f, 0.0f, 1.0f, 0.5f, -0.5f, -0.5f, 1.0f, 1.0f,
        0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.5f, -0.5f, 0.5f, 1.0f, 0.0f,
        -0.5f, -0.5f, 0.5f, 0.0f, 0.0f, -0.5f, -0.5f, -0.5f, 0.0f, 1.0f,

        -0.5f, 0.5f, -0.5f, 0.0f, 1.0f, 0.5f, 0.5f, -0.5f, 1.0f, 1.0f,
        0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f,
        -0.5f, 0.5f, 0.5f, 0.0f, 0.0f, -0.5f, 0.5f, -0.5f, 0.0f, 1.0f};

    GLuint vbo;
    glGenBuffers(1, &vbo);

    glGenVertexArrays(1, &state.vao);

    glBindVertexArray(state.vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);

    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(5);

    glEnable(GL_DEPTH_TEST);

    // Start Session
    XrSessionActionSetsAttachInfo actionset_attach_info = {
        .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
        .countActionSets = 1,
        .actionSets = &state.gameplay_actionset,
    };

    result = xrAttachSessionActionSets(state.session, &actionset_attach_info);
    if (result != XR_SUCCESS)
    {
        printf("Failed to attach action set\n");
        return 1;
    }

    XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
    int quit_mainloop = 0;
    int session_running = 0; // to avoid beginning an already running session
    int run_framecycle = 0;  // for some session states skip the frame cycle
    while (!quit_mainloop)
    {
        // Pump SDL events
        SDL_Event sdl_event;
        while (SDL_PollEvent(&sdl_event))
        {
            if (sdl_event.type == SDL_QUIT)
            {
                printf("Requesting exit...\n");
                xrRequestExitSession(state.session);
            }
        }

        // Handle runtime Events
        // we do this before xrWaitFrame() so we can go idle or
        // break out of the main render loop as early as possible and don't have to
        // uselessly render or submit one. Calling xrWaitFrame commits you to
        // calling xrBeginFrame eventually.
        XrEventDataBuffer runtime_event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        XrResult poll_result = xrPollEvent(state.instance, &runtime_event);
        while (poll_result == XR_SUCCESS)
        {
            switch (runtime_event.type)
            {
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            {
                XrEventDataInstanceLossPending *event = (XrEventDataInstanceLossPending *)&runtime_event;
                printf("EVENT: instance loss pending at %lld! Destroying instance.\n", event->lossTime);
                quit_mainloop = 1;
                continue;
            }
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            {
                XrEventDataSessionStateChanged *event = (XrEventDataSessionStateChanged *)&runtime_event;
                printf("EVENT: session state changed from %d to %d\n", session_state, event->state);
                session_state = event->state;

                /*
				 * react to session state changes, see OpenXR spec 9.3 diagram. What we need to react to:
				 *
				 * * READY -> xrBeginSession STOPPING -> xrEndSession (note that the same session can be restarted)
				 * * EXITING -> xrDestroySession (EXITING only happens after we went through STOPPING and called xrEndSession)
				 *
				 * After exiting it is still possible to create a new session but we don't do that here.
				 *
				 * * IDLE -> don't run render loop, but keep polling for events
				 * * SYNCHRONIZED, VISIBLE, FOCUSED -> run render loop
				 */
                switch (session_state)
                {
                // skip render loop, keep polling
                case XR_SESSION_STATE_MAX_ENUM: // must be a bug
                case XR_SESSION_STATE_IDLE:
                case XR_SESSION_STATE_UNKNOWN:
                {
                    run_framecycle = 0;
                    break; // state handling switch
                }

                // do nothing, run render loop normally
                case XR_SESSION_STATE_FOCUSED:
                case XR_SESSION_STATE_SYNCHRONIZED:
                case XR_SESSION_STATE_VISIBLE:
                {
                    run_framecycle = 1;
                    break; // state handling switch
                }

                // begin session and then run render loop
                case XR_SESSION_STATE_READY:
                {
                    // start session only if it is not running, i.e. not when we already called xrBeginSession
                    // but the runtime did not switch to the next state yet
                    if (!session_running)
                    {
                        XrSessionBeginInfo session_begin_info = {
                            .type = XR_TYPE_SESSION_BEGIN_INFO,
                            .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                        };
                        result = xrBeginSession(state.session, &session_begin_info);
                        if (result != XR_SUCCESS)
                        {
                            printf("Failed to begin session\n");
                            return 1;
                        }
                        session_running = 1;
                    }
                    // after beginning the session, run render loop
                    run_framecycle = 1;
                    break; // state handling switch
                }

                // end session, skip render loop, keep polling for next state change
                case XR_SESSION_STATE_STOPPING:
                {
                    // end session only if it is running, i.e. not when we already called xrEndSession but the
                    // runtime did not switch to the next state yet
                    if (session_running)
                    {
                        result = xrEndSession(state.session);
                        if (result != XR_SUCCESS)
                        {
                            printf("Failed to end session\n");
                            return 1;
                        }
                        session_running = 0;
                    }
                    // after ending the session, don't run render loop
                    run_framecycle = 0;

                    break; // state handling switch
                }

                // destroy session, skip render loop, exit render loop and quit
                case XR_SESSION_STATE_LOSS_PENDING:
                case XR_SESSION_STATE_EXITING:
                    result = xrDestroySession(state.session);
                    if (result != XR_SUCCESS)
                    {
                        printf("Failed to destroy session\n");
                        return 1;
                    }
                    quit_mainloop = 1;
                    run_framecycle = 0;

                    break; // state handling switch
                }
                break; // session event handling switch
            }
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            {
                printf("EVENT: interaction profile changed!\n");
                XrEventDataInteractionProfileChanged *event = (XrEventDataInteractionProfileChanged *)&runtime_event;
                (void)event;

                for (int i = 0; i < HAND_COUNT; i++)
                {
                    XrInteractionProfileState profile_state;
                    XrResult res = xrGetCurrentInteractionProfile(state.session, state.hand_paths[i], &profile_state);
                    if (result != XR_SUCCESS)
                    {
                        printf("Failed to get interaction profile\n");
                        return 1;
                    }

                    XrPath prof = profile_state.interactionProfile;

                    uint32_t strl;
                    char profile_str[XR_MAX_PATH_LENGTH];
                    res = xrPathToString(state.instance, prof, XR_MAX_PATH_LENGTH, &strl, profile_str);
                    if (result != XR_SUCCESS)
                    {
                        printf("Failed to get profile string\n");
                        return 1;
                    }

                    printf("Event: Interaction profile changed for %d: %s\n", i, profile_str);
                }
                break;
            }
            default:
                printf("Unhandled event (type %d)\n", runtime_event.type);
            }

            runtime_event.type = XR_TYPE_EVENT_DATA_BUFFER;
            poll_result = xrPollEvent(state.instance, &runtime_event);
        }
        if (poll_result == XR_EVENT_UNAVAILABLE)
        {
            // processed all events in the queue
        }
        else
        {
            printf("Failed to poll events!\n");
            break;
        }

        if (!run_framecycle)
        {
            continue;
        }

        // Wait for our turn to do head-pose dependent computation and render a frame
        XrFrameState frame_state = {.type = XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo frame_wait_info = {.type = XR_TYPE_FRAME_WAIT_INFO};
        result = xrWaitFrame(state.session, &frame_wait_info, &frame_state);
        if (result != XR_SUCCESS)
        {
            printf("Failed to wait frame\n");
            return 1;
        }

        // Create view, projection matrices
        XrViewLocateInfo view_locate_info = {
            .type = XR_TYPE_VIEW_LOCATE_INFO,
            .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            .displayTime = frame_state.predictedDisplayTime,
            .space = state.play_space,
        };

        XrViewState view_state = {.type = XR_TYPE_VIEW_STATE};
        result = xrLocateViews(state.session, &view_locate_info, &view_state, state.view_count, &state.view_count, state.views);
        if (result != XR_SUCCESS)
        {
            printf("Failed to locate views\n");
            return 1;
        }

        //! @todo Move this action processing to before xrWaitFrame, probably.
        const XrActiveActionSet active_actionsets[] = {
            {
                .actionSet = state.gameplay_actionset,
                .subactionPath = XR_NULL_PATH,
            },
        };

        XrActionsSyncInfo actions_sync_info = {
            .type = XR_TYPE_ACTIONS_SYNC_INFO,
            .countActiveActionSets = sizeof(active_actionsets) / sizeof(active_actionsets[0]),
            .activeActionSets = active_actionsets,
        };
        result = xrSyncActions(state.session, &actions_sync_info);
        if (result != XR_SUCCESS)
        {
            printf("Failed to sync actions\n");
            // return 1;
        }

        // query each value / location with a subaction path != XR_NULL_PATH
        // resulting in individual values per hand/.
        XrActionStateFloat grab_value[HAND_COUNT];
        XrSpaceLocation hand_locations[HAND_COUNT];

        for (int i = 0; i < HAND_COUNT; i++)
        {
            XrActionStatePose hand_pose_state = {.type = XR_TYPE_ACTION_STATE_POSE};
            {
                XrActionStateGetInfo get_info = {
                    .type = XR_TYPE_ACTION_STATE_GET_INFO,
                    .action = state.hand_pose_action,
                    .subactionPath = state.hand_paths[i],
                };
                result = xrGetActionStatePose(state.session, &get_info, &hand_pose_state);
                if (result != XR_SUCCESS)
                {
                    printf("Failed to get pose action\n");
                    // return 1;
                }
            }

            hand_locations[i].type = XR_TYPE_SPACE_LOCATION;
            hand_locations[i].next = NULL;

            result = xrLocateSpace(state.hand_pose_spaces[i], state.play_space, frame_state.predictedDisplayTime, &hand_locations[i]);
            if (result != XR_SUCCESS)
            {
                printf("Failed to locate space \n");
                // return 1;
            }

            /*
			printf("Pose %d valid %d: %f %f %f %f, %f %f %f\n", i,
			spaceLocationValid[i], spaceLocation[0].pose.orientation.x,
			spaceLocation[0].pose.orientation.y, spaceLocation[0].pose.orientation.z,
			spaceLocation[0].pose.orientation.w, spaceLocation[0].pose.position.x,
			spaceLocation[0].pose.position.y, spaceLocation[0].pose.position.z
			);
			*/

            grab_value[i].type = XR_TYPE_ACTION_STATE_FLOAT;
            grab_value[i].next = NULL;
            {
                XrActionStateGetInfo get_info = {
                    .type = XR_TYPE_ACTION_STATE_GET_INFO,
                    .action = state.grab_action_float,
                    .subactionPath = state.hand_paths[i],
                };

                result = xrGetActionStateFloat(state.session, &get_info, &grab_value[i]);
                if (result != XR_SUCCESS)
                {
                    printf("Failed to get grab action \n");
                    // return 1;
                }
            }

            // printf("Grab %d active %d, current %f, changed %d\n", i,
            // grabValue[i].isActive, grabValue[i].currentState,
            // grabValue[i].changedSinceLastSync);

            if (grab_value[i].isActive && grab_value[i].currentState > 0.75)
            {
                XrHapticVibration vibration = {
                    .type = XR_TYPE_HAPTIC_VIBRATION,
                    .amplitude = 0.5,
                    .duration = XR_MIN_HAPTIC_DURATION,
                    .frequency = XR_FREQUENCY_UNSPECIFIED,
                };

                XrHapticActionInfo haptic_action_info = {
                    .type = XR_TYPE_HAPTIC_ACTION_INFO,
                    .action = state.haptic_action,
                    .subactionPath = state.hand_paths[i],
                };

                result = xrApplyHapticFeedback(state.session, &haptic_action_info, (const XrHapticBaseHeader *)&vibration);
                if (result != XR_SUCCESS)
                {
                    printf("Failed to apply haptics\n");
                    // return 1;
                }
                // printf("Sent haptic output to hand %d\n", i);
            }
        };

        // Begin frame
        XrFrameBeginInfo frame_begin_info = {.type = XR_TYPE_FRAME_BEGIN_INFO};
        result = xrBeginFrame(state.session, &frame_begin_info);
        if (result != XR_SUCCESS)
        {
            printf("Failed to begin frame\n");
            break;
        }

        // Render each eye and fill projection_views with the result
        for (int i = 0; i < state.view_count; i++)
        {

            if (!frame_state.shouldRender)
            {
                printf("shouldRender = false, Skipping rendering work\n");
                continue;
            }

            int w = state.view_confs[i].recommendedImageRectWidth;
            int h = state.view_confs[i].recommendedImageRectHeight;

            float proj[16];
            mat4_proj_xr(proj, state.views[i].fov, state.near_z, state.far_z);

            float translation[16];
            mat4_identity(translation);
            mat4_translate(translation, translation, (float *)&state.views[i].pose.position);

            float rotation[16];
            mat4_rotation_quat(rotation, (float *)&state.views[i].pose.orientation);

            float view[16];
            mat4_multiply(view, translation, rotation);
            mat4_inverse(view, view);

            uint32_t acquired_index;
            XrSwapchainImageAcquireInfo acquire_info = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            result = xrAcquireSwapchainImage(state.swapchains[i], &acquire_info, &acquired_index);
            if (result != XR_SUCCESS)
            {
                printf("Failed to acquire swapchain image\n");
                break;
            }

            XrSwapchainImageWaitInfo wait_info = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = 1000};
            result = xrWaitSwapchainImage(state.swapchains[i], &wait_info);
            if (result != XR_SUCCESS)
            {
                printf("Failed to wait for swapchain image\n");
                break;
            }

            uint32_t depth_acquired_index = UINT32_MAX;
            XrSwapchainImageAcquireInfo depth_acquire_info = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            result = xrAcquireSwapchainImage(state.depths[i], &depth_acquire_info, &depth_acquired_index);
            if (result != XR_SUCCESS)
            {
                printf("Failed to acquire swapchain image\n");
                break;
            }

            XrSwapchainImageWaitInfo depth_wait_info = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = 1000};
            result = xrWaitSwapchainImage(state.depths[i], &depth_wait_info);
            if (result != XR_SUCCESS)
            {
                printf("Failed to wait for swapchain image\n");
                break;
            }

            state.proj_views[i].pose = state.views[i].pose;
            state.proj_views[i].fov = state.views[i].fov;

            GLuint framebuffer = state.framebuffers[i][acquired_index];
            GLuint swap_image = state.swapchain_images[i][acquired_index].image;
            GLuint depth_image = state.depth_images[i][depth_acquired_index].image;

            render_frame(w, h, frame_state.predictedDisplayTime, i, hand_locations, proj, view, framebuffer, swap_image, depth_image);

            XrSwapchainImageReleaseInfo release_info = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, .next = NULL};
            result = xrReleaseSwapchainImage(state.swapchains[i], &release_info);
            if (result != XR_SUCCESS)
            {
                printf("Failed to release for swapchain image\n");
                break;
            }

            XrSwapchainImageReleaseInfo depth_release_info = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            result = xrReleaseSwapchainImage(state.depths[i], &depth_release_info);
            if (result != XR_SUCCESS)
            {
                printf("Failed to release for swapchain image\n");
                break;
            }
        }

        XrCompositionLayerProjection projection_layer = {
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
            .space = state.play_space,
            .viewCount = state.view_count,
            .views = state.proj_views,
        };

        int submitted_layer_count = 1;
        const XrCompositionLayerBaseHeader *submitted_layers[1] = {(const XrCompositionLayerBaseHeader *const)&projection_layer};

        if ((view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0)
        {
            printf("submitting 0 layers because orientation is invalid\n");
            submitted_layer_count = 0;
        }

        if (!frame_state.shouldRender)
        {
            printf("submitting 0 layers because shouldRender = false\n");
            submitted_layer_count = 0;
        }

        XrFrameEndInfo frame_end_info = {
            .type = XR_TYPE_FRAME_END_INFO,
            .displayTime = frame_state.predictedDisplayTime,
            .layerCount = submitted_layer_count,
            .layers = submitted_layers,
            .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        };

        result = xrEndFrame(state.session, &frame_end_info);
        if (result != XR_SUCCESS)
        {
            printf("Failed to end frame\n");
            break;
        }
    }

    // Cleanup
    for (int i = 0; i < state.view_count; i++)
    {
        glDeleteFramebuffers(state.swapchain_lengths[i], state.framebuffers[i]);
    }

    xrDestroyInstance(state.instance);

    return 0;
}
