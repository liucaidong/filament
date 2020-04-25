/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iostream>
#include <string>
#include <map>
#include <vector>

#include <getopt/getopt.h>

#include <imgui.h>
#include <filagui/ImGuiExtensions.h>

#include <utils/Path.h>

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Exposure.h>
#include <filament/DebugRegistry.h>
#include <filament/IndirectLight.h>
#include <filament/IndexBuffer.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/TransformManager.h>
#include <filament/View.h>
#include <filament/VertexBuffer.h>

#include <math/mat3.h>
#include <math/mat4.h>
#include <math/vec4.h>
#include <math/norm.h>

#include <filamentapp/Config.h>
#include <filamentapp/IBL.h>
#include <filamentapp/FilamentApp.h>
#include <filamentapp/MeshAssimp.h>

#include "material_sandbox.h"

using namespace filament::math;
using namespace filament;
using namespace filamat;
using namespace utils;

static std::vector<Path> g_filenames;

static Scene* g_scene = nullptr;

std::unique_ptr<MeshAssimp> g_meshSet;
static std::map<std::string, MaterialInstance*> g_meshMaterialInstances;
static SandboxParameters g_params;
static Config g_config;
static bool g_shadowPlane = false;
static bool g_singleMode = false;

static void printUsage(char* name) {
    std::string exec_name(Path(name).getName());
    std::string usage(
            "SAMPLE_MATERIAL showcases all material models\n"
            "Usage:\n"
            "    SAMPLE_MATERIAL [options] <mesh files (.obj, .fbx)>\n"
            "Options:\n"
            "   --help, -h\n"
            "       Prints this message\n\n"
            "   --api, -a\n"
            "       Specify the backend API: opengl (default), vulkan, or metal\n\n"
            "   --ibl=<path to cmgen IBL>, -i <path>\n"
            "       Applies an IBL generated by cmgen's deploy option\n\n"
            "   --split-view, -v\n"
            "       Splits the window into 4 views\n\n"
            "   --scale=[number], -s [number]\n"
            "       Applies uniform scale\n\n"
            "   --shadow-plane, -p\n"
            "       Enable shadow plane\n\n"
            "   --single\n"
            "       Only apply the edited material to the first renderable in the scene\n\n"
            "   --dirt\n"
            "       Specify a dirt texture\n\n"
            "   --camera=<camera mode>, -c <camera mode>\n"
            "       Set the camera mode: orbit (default) or flight\n\n"
    );
    const std::string from("SAMPLE_MATERIAL");
    for (size_t pos = usage.find(from); pos != std::string::npos; pos = usage.find(from, pos)) {
        usage.replace(pos, from.length(), exec_name);
    }
    std::cout << usage;
}

static int handleCommandLineArgments(int argc, char* argv[], Config* config) {
    static constexpr const char* OPTSTR = "ha:vps:i:d:c:";
    static const struct option OPTIONS[] = {
            { "help",         no_argument,       nullptr, 'h' },
            { "api",          required_argument, nullptr, 'a' },
            { "ibl",          required_argument, nullptr, 'i' },
            { "split-view",   no_argument,       nullptr, 'v' },
            { "scale",        required_argument, nullptr, 's' },
            { "shadow-plane", no_argument,       nullptr, 'p' },
            { "single",       no_argument,       nullptr, 'n' },
            { "dirt",         required_argument, nullptr, 'd' },
            { "camera",       required_argument, nullptr, 'c' },
            { nullptr, 0, nullptr, 0 }  // termination of the option list
    };
    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, OPTSTR, OPTIONS, &option_index)) >= 0) {
        std::string arg(optarg ? optarg : "");
        switch (opt) {
            default:
            case 'h':
                printUsage(argv[0]);
                exit(0);
            case 'a':
                if (arg == "opengl") {
                    config->backend = Engine::Backend::OPENGL;
                } else if (arg == "vulkan") {
                    config->backend = Engine::Backend::VULKAN;
                } else if (arg == "metal") {
                    config->backend = Engine::Backend::METAL;
                } else {
                    std::cerr << "Unrecognized backend. Must be 'opengl'|'vulkan'|'metal'." << std::endl;
                }
                break;
            case 'c':
                if (arg == "flight") {
                    config->cameraMode = camutils::Mode::FREE_FLIGHT;
                } else if (arg == "orbit") {
                    config->cameraMode = camutils::Mode::ORBIT;
                } else {
                    std::cerr << "Unrecognized camera mode. Must be 'flight'|'orbit'.\n";
                }
                break;
            case 'i':
                config->iblDirectory = arg;
                break;
            case 's':
                try {
                    config->scale = std::stof(arg);
                } catch (std::invalid_argument& e) {
                    // keep scale of 1.0
                } catch (std::out_of_range& e) {
                    // keep scale of 1.0
                }
                break;
            case 'v':
                config->splitView = true;
                break;
            case 'p':
                g_shadowPlane = true;
                break;
            case 'n':
                g_singleMode = true;
                break;
            case 'd':
                config->dirt =  arg;
                break;
        }
    }

    return optind;
}

static void cleanup(Engine* engine, View*, Scene*) {
    for (const auto& material : g_meshMaterialInstances) {
        engine->destroy(material.second);
    }

    for (auto& i : g_params.materialInstance) {
        engine->destroy(i);
    }

    for (auto& i : g_params.material) {
        engine->destroy(i);
    }

    g_meshSet.reset(nullptr);

    engine->destroy(g_params.light);
    engine->destroy(g_params.spotLight);

    EntityManager& em = EntityManager::get();
    em.destroy(g_params.light);
    em.destroy(g_params.spotLight);
}

static void setup(Engine* engine, View*, Scene* scene) {
    g_scene = scene;

    g_meshSet = std::make_unique<MeshAssimp>(*engine);

    createInstances(g_params, *engine);

    for (auto& filename : g_filenames) {
        g_meshSet->addFromFile(filename, g_meshMaterialInstances);
    }

    auto& tcm = engine->getTransformManager();
    auto ei = tcm.getInstance(g_meshSet->getRenderables()[0]);
    tcm.setTransform(ei, mat4f{ mat3f(g_config.scale), float3(0.0f, 0.0f, -4.0f) } *
            tcm.getWorldTransform(ei));

    size_t count = 0;
    auto& rcm = engine->getRenderableManager();
    for (auto renderable : g_meshSet->getRenderables()) {
        auto instance = rcm.getInstance(renderable);
        if (!instance) continue;

        rcm.setCastShadows(instance, g_params.castShadows);
        rcm.setScreenSpaceContactShadows(instance, true);

        if (!g_singleMode || count == 0) {
            for (size_t i = 0; i < rcm.getPrimitiveCount(instance); i++) {
                rcm.setMaterialInstanceAt(instance, i, g_params.materialInstance[MATERIAL_LIT]);
            }
        } else {
            auto ei = tcm.getInstance(renderable);
            tcm.setTransform(ei, mat4f{ mat3f(g_config.scale), float3(0.0f, 0.0f, -3.0f) } *
                    tcm.getWorldTransform(ei));
        }
        count++;

        scene->addEntity(renderable);
    }

    scene->addEntity(g_params.light);

    // Parent the spot light to the root renderable in the mesh.
    tcm.create(g_params.spotLight, tcm.getInstance(g_meshSet->getRenderables()[0]));
    g_params.spotLightPosition = float3{0.0, 1.0, 0.0f};

    if (g_shadowPlane) {
        EntityManager& em = EntityManager::get();
        Material* shadowMaterial = Material::Builder()
                .package(RESOURCES_GROUNDSHADOW_DATA, RESOURCES_GROUNDSHADOW_SIZE)
                .build(*engine);
        shadowMaterial->setDefaultParameter("strength", 0.7f);

        const static uint32_t indices[] = {
                0, 1, 2, 2, 3, 0
        };

        const static filament::math::float3 vertices[] = {
                { -10, 0, -10 },
                { -10, 0,  10 },
                {  10, 0,  10 },
                {  10, 0, -10 },
        };

        short4 tbn = filament::math::packSnorm16(
                mat3f::packTangentFrame(
                        filament::math::mat3f{
                                float3{ 1.0f, 0.0f, 0.0f },
                                float3{ 0.0f, 0.0f, 1.0f },
                                float3{ 0.0f, 1.0f, 0.0f }
                        }
                ).xyzw);

        const static filament::math::short4 normals[] { tbn, tbn, tbn, tbn };

        VertexBuffer* vertexBuffer = VertexBuffer::Builder()
                .vertexCount(4)
                .bufferCount(2)
                .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3)
                .attribute(VertexAttribute::TANGENTS, 1, VertexBuffer::AttributeType::SHORT4)
                .normalized(VertexAttribute::TANGENTS)
                .build(*engine);

        vertexBuffer->setBufferAt(*engine, 0, VertexBuffer::BufferDescriptor(
                vertices, vertexBuffer->getVertexCount() * sizeof(vertices[0])));
        vertexBuffer->setBufferAt(*engine, 1, VertexBuffer::BufferDescriptor(
                normals, vertexBuffer->getVertexCount() * sizeof(normals[0])));

        IndexBuffer* indexBuffer = IndexBuffer::Builder()
                .indexCount(6)
                .build(*engine);

        indexBuffer->setBuffer(*engine, IndexBuffer::BufferDescriptor(
                indices, indexBuffer->getIndexCount() * sizeof(uint32_t)));

        Entity planeRenderable = em.create();
        RenderableManager::Builder(1)
                .boundingBox({{ 0, 0, 0 },
                              { 10, 1e-4f, 10 }})
                .material(0, shadowMaterial->getDefaultInstance())
                .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                        vertexBuffer, indexBuffer, 0, 6)
                .culling(false)
                .receiveShadows(true)
                .castShadows(false)
                .build(*engine, planeRenderable);

        scene->addEntity(planeRenderable);

        tcm.setTransform(tcm.getInstance(planeRenderable),
                filament::math::mat4f::translation(float3{ 0, -1, -4 }));
    }

    auto* ibl = FilamentApp::get().getIBL();
    if (ibl) {
        auto& params = g_params;
        IndirectLight* const pIndirectLight = ibl->getIndirectLight();
        params.lightDirection = IndirectLight::getDirectionEstimate(ibl->getSphericalHarmonics());
        float4 c = pIndirectLight->getColorEstimate(ibl->getSphericalHarmonics(), params.lightDirection);
        params.lightIntensity = c.w * pIndirectLight->getIntensity();
        params.lightColor = c.rgb;
    }

    g_params.bloomOptions.dirt = FilamentApp::get().getDirtTexture();
}

static filament::MaterialInstance* updateInstances(
        SandboxParameters& params, filament::Engine& engine) {

    int material = params.currentMaterialModel;
    if (material == MATERIAL_MODEL_LIT) {
        if (params.currentBlending == BLENDING_TRANSPARENT) material = MATERIAL_TRANSPARENT;
        if (params.currentBlending == BLENDING_FADE) material = MATERIAL_FADE;
        if (params.ssr) {
            if (params.currentBlending == BLENDING_THIN_REFRACTION) material = MATERIAL_THIN_SS_REFRACTION;
            if (params.currentBlending == BLENDING_SOLID_REFRACTION) material = MATERIAL_SOLID_SS_REFRACTION;
        } else {
            if (params.currentBlending == BLENDING_THIN_REFRACTION) material = MATERIAL_THIN_REFRACTION;
            if (params.currentBlending == BLENDING_SOLID_REFRACTION) material = MATERIAL_SOLID_REFRACTION;
        }
    }

    bool hasRefraction = params.currentBlending == BLENDING_THIN_REFRACTION ||
            params.currentBlending == BLENDING_SOLID_REFRACTION;

    MaterialInstance* materialInstance = params.materialInstance[material];
    materialInstance->setParameter("baseColor", RgbType::sRGB, params.color);

    if (params.currentMaterialModel != MATERIAL_MODEL_CLOTH) {
        math::float4 emissive(Color::toLinear(params.emissiveColor), params.emissiveExposureWeight);
        emissive.rgb *= Exposure::luminance(params.emissiveEV);
        materialInstance->setParameter("emissive", emissive);
    }

    if (params.currentMaterialModel == MATERIAL_MODEL_LIT) {
        materialInstance->setParameter("roughness", params.roughness);
        materialInstance->setParameter("metallic", params.metallic);
        if (!hasRefraction) {
            materialInstance->setParameter("reflectance", params.reflectance);
        }
        materialInstance->setParameter("clearCoat", params.clearCoat);
        materialInstance->setParameter("clearCoatRoughness", params.clearCoatRoughness);
        materialInstance->setParameter("anisotropy", params.anisotropy);

        if (params.currentBlending != BLENDING_OPAQUE) {
            materialInstance->setParameter("alpha", params.alpha);
        }

        if  (hasRefraction) {
            math::float3 color = Color::toLinear(params.transmittanceColor);
            materialInstance->setParameter("absorption",
                    Color::absorptionAtDistance(color, params.distance));
            materialInstance->setParameter("ior", params.ior);
            materialInstance->setParameter("transmission", params.transmission);
            materialInstance->setParameter("thickness", params.thickness);
        }
    }

    if (params.currentMaterialModel == MATERIAL_MODEL_SPECGLOSS) {
        materialInstance->setParameter("glossiness", params.glossiness);
        materialInstance->setParameter("specularColor", params.specularColor);
        materialInstance->setParameter("reflectance", params.reflectance);
        materialInstance->setParameter("clearCoat", params.clearCoat);
        materialInstance->setParameter("clearCoatRoughness", params.clearCoatRoughness);
        materialInstance->setParameter("anisotropy", params.anisotropy);
    }

    if (params.currentMaterialModel == MATERIAL_MODEL_SUBSURFACE) {
        materialInstance->setParameter("roughness", params.roughness);
        materialInstance->setParameter("metallic", params.metallic);
        materialInstance->setParameter("reflectance", params.reflectance);
        materialInstance->setParameter("thickness", params.thickness);
        materialInstance->setParameter("subsurfacePower", params.subsurfacePower);
        materialInstance->setParameter("subsurfaceColor", RgbType::sRGB, params.subsurfaceColor);
    }

    if (params.currentMaterialModel == MATERIAL_MODEL_CLOTH) {
        materialInstance->setParameter("roughness", params.roughness);
        materialInstance->setParameter("sheenColor", RgbType::sRGB, params.sheenColor);
        materialInstance->setParameter("subsurfaceColor", RgbType::sRGB, params.subsurfaceColor);
    }

    if (params.currentMaterialModel != MATERIAL_MODEL_UNLIT) {
        materialInstance->setSpecularAntiAliasingVariance(params.specularAntiAliasingVariance);
        materialInstance->setSpecularAntiAliasingThreshold(params.specularAntiAliasingThreshold);
    }

    return materialInstance;
}

static void gui(filament::Engine* engine, filament::View*) {
    auto& params = g_params;
    ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f));
    ImGui::Begin("Parameters");
    {
        if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Combo("Model", &params.currentMaterialModel,
                    "Unlit\0Lit\0Subsurface\0Cloth\0Specular glossiness\0\0");

            if (params.currentMaterialModel == MATERIAL_MODEL_LIT) {
                ImGui::Combo("Blending", &params.currentBlending,
                        "Opaque\0Transparent\0Fade\0Thin refraction\0Solid refraction\0\0");
            }

            ImGui::ColorEdit3("Base color", &params.color.r);

            bool hasRefraction = params.currentBlending == BLENDING_THIN_REFRACTION ||
                    params.currentBlending == BLENDING_SOLID_REFRACTION;

            if (params.currentMaterialModel > MATERIAL_MODEL_UNLIT) {
                if (params.currentBlending == BLENDING_TRANSPARENT ||
                        params.currentBlending == BLENDING_FADE) {
                    ImGui::SliderFloat("Alpha", &params.alpha, 0.0f, 1.0f);
                }

                if (params.currentMaterialModel != MATERIAL_MODEL_SPECGLOSS) {
                    ImGui::SliderFloat("Roughness", &params.roughness, 0.0f, 1.0f);
                } else {
                    ImGui::SliderFloat("Glossiness", &params.glossiness, 0.0f, 1.0f);
                    ImGui::ColorEdit3("Specular color", &params.specularColor.r);
                }

                if (params.currentMaterialModel != MATERIAL_MODEL_CLOTH &&
                        params.currentMaterialModel != MATERIAL_MODEL_SPECGLOSS) {
                    if (!hasRefraction) {
                        ImGui::SliderFloat("Metallic", &params.metallic, 0.0f, 1.0f);
                        ImGui::SliderFloat("Reflectance", &params.reflectance, 0.0f, 1.0f);
                    }
                }

                if (params.currentMaterialModel != MATERIAL_MODEL_CLOTH &&
                        params.currentMaterialModel != MATERIAL_MODEL_SUBSURFACE) {
                    ImGui::SliderFloat("Clear coat", &params.clearCoat, 0.0f, 1.0f);
                    ImGui::SliderFloat("Clear coat roughness", &params.clearCoatRoughness, 0.0f, 1.0f);
                    ImGui::SliderFloat("Anisotropy", &params.anisotropy, -1.0f, 1.0f);
                }

                if (params.currentMaterialModel == MATERIAL_MODEL_SUBSURFACE) {
                    ImGui::SliderFloat("Thickness", &params.thickness, 0.0f, 1.0f);
                    ImGui::SliderFloat("Subsurface power", &params.subsurfacePower, 1.0f, 24.0f);
                    ImGui::ColorEdit3("Subsurface color", &params.subsurfaceColor.r);
                }

                if (params.currentMaterialModel == MATERIAL_MODEL_CLOTH) {
                    ImGui::ColorEdit3("Sheen color", &params.sheenColor.r);
                    ImGui::ColorEdit3("Subsurface color", &params.subsurfaceColor.r);
                }

                if (hasRefraction) {
                    ImGui::SliderFloat("IOR", &params.ior, 1.0f, 3.0f);
                    ImGui::SliderFloat("Transmission", &params.transmission, 0.0f, 1.0f);
                    ImGui::SliderFloat("Thickness", &params.thickness, 0.0f, 1.0f);
                    ImGui::ColorEdit3("Transmittance", &params.transmittanceColor.r);
                    ImGui::SliderFloat("Distance", &params.distance, 0.0f, 4.0f);
                    ImGui::Checkbox("Screen space refraction", &params.ssr);
                }
            }

            ImGui::ColorEdit3("Emissive color", &params.emissiveColor.r);
            ImGui::SliderFloat("Emissive EV", &params.emissiveEV, -24.0f, 24.0f);
            ImGui::SliderFloat("Exposure weight", &params.emissiveExposureWeight, 0.0f, 1.0f);
        }

        if (ImGui::CollapsingHeader("Shading AA")) {
            ImGui::SliderFloat("Variance", &params.specularAntiAliasingVariance, 0.0f, 1.0f);
            ImGui::SliderFloat("Threshold", &params.specularAntiAliasingThreshold, 0.0f, 1.0f);
        }

        if (ImGui::CollapsingHeader("Object")) {
            ImGui::Checkbox("Cast shadows###object", &params.castShadows);
        }

        if (ImGui::CollapsingHeader("Camera")) {
            ImGui::SliderFloat("Focal length", &FilamentApp::get().getCameraFocalLength(), 16.0f, 90.0f);
            ImGui::SliderFloat("Aperture", &params.cameraAperture, 1.0f, 32.0f);
            ImGui::SliderFloat("Speed", &params.cameraSpeed, 800.0f, 1.0f);
            ImGui::SliderFloat("ISO", &params.cameraISO, 25.0f, 6400.0f);
        }

        if (ImGui::CollapsingHeader("Indirect Light")) {
            ImGui::SliderFloat("IBL", &params.iblIntensity, 0.0f, 50000.0f);
            ImGui::SliderAngle("Rotation", &params.iblRotation);
            ImGui::Indent();
            if (ImGui::CollapsingHeader("SSAO")) {
                DebugRegistry& debug = engine->getDebugRegistry();
                ImGui::Checkbox("Enabled###ssao", &params.ssao);
                ImGui::SliderFloat("Radius", &params.ssaoOptions.radius, 0.05f, 5.0f);
                ImGui::SliderFloat("Bias", &params.ssaoOptions.bias, 0.0f, 0.01f, "%.6f");
                ImGui::SliderFloat("Intensity", &params.ssaoOptions.intensity, 0.0f, 4.0f);
                ImGui::SliderFloat("Power", &params.ssaoOptions.power, 0.0f, 4.0f);
            }
            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader("Directional Light")) {
            ImGui::Checkbox("Enabled###directionalLight", &params.directionalLightEnabled);
            ImGui::ColorEdit3("Color", &params.lightColor.r);
            ImGui::SliderFloat("Lux", &params.lightIntensity, 0.0f, 150000.0f);
            ImGui::SliderFloat("Sun size", &params.sunAngularRadius, 0.1f, 10.0f);
            ImGui::SliderFloat("Halo size", &params.sunHaloSize, 1.01f, 40.0f);
            ImGui::SliderFloat("Halo falloff", &params.sunHaloFalloff, 0.0f, 2048.0f);
            ImGuiExt::DirectionWidget("Direction", params.lightDirection.v);
            ImGui::Indent();
            if (ImGui::CollapsingHeader("Contact Shadows")) {
                DebugRegistry& debug = engine->getDebugRegistry();
                ImGui::Checkbox("Enabled###contactShadows", &params.screenSpaceContactShadows);
                ImGui::SliderInt("Steps", &params.stepCount, 0, 255);
                ImGui::SliderFloat("Distance", &params.maxShadowDistance, 0.0f, 10.0f);
            }
            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader("Spot Light")) {
            ImGui::Checkbox("Enabled###spotLight", &params.spotLightEnabled);
            ImGui::SliderFloat3("Position", &params.spotLightPosition.x, -5.0f, 5.0f);
            ImGui::ColorEdit3("Color", &params.spotLightColor.r);
            ImGui::Checkbox("Cast shadows", &params.spotLightCastShadows);
            ImGui::SliderFloat("Lumens", &params.spotLightIntensity, 0.0, 1000000.f);
            ImGui::SliderAngle("Cone angle", &params.spotLightConeAngle, 0.0f, 90.0f);
            ImGui::SliderFloat("Cone fade", &params.spotLightConeFade, 0.0f, 1.0f);
        }

        if (ImGui::CollapsingHeader("Fog")) {
            ImGui::Checkbox("Enable Fog", &params.fogOptions.enabled);
            ImGui::SliderFloat("Start", &params.fogOptions.distance, 0.0f, 100.0f);
            ImGui::SliderFloat("Density", &params.fogOptions.density, 0.0f, 1.0f);
            ImGui::SliderFloat("Height", &params.fogOptions.height, 0.0f, 100.0f);
            ImGui::SliderFloat("Height Falloff", &params.fogOptions.heightFalloff, 0.0f, 10.0f);
            ImGui::SliderFloat("Scattering Start", &params.fogOptions.inScatteringStart, 0.0f, 100.0f);
            ImGui::SliderFloat("Scattering Size", &params.fogOptions.inScatteringSize, 0.0f, 100.0f);
            ImGui::Checkbox("Color from IBL", &params.fogOptions.fogColorFromIbl);
            ImGui::ColorPicker3("Color", params.fogOptions.color.v);
        }

        if (ImGui::CollapsingHeader("Post-processing")) {
            ImGui::Checkbox("MSAA 4x", &params.msaa);
            ImGui::Checkbox("Tone mapping", &params.tonemapping);
            ImGui::Indent();
                ImGui::Checkbox("Bloom", &params.bloomOptions.enabled);
                if (params.bloomOptions.enabled) {
                    ImGui::SliderFloat("Strength", &params.bloomOptions.strength, 0.0f, 1.0f);
                    ImGui::SliderFloat("Dirt", &params.bloomOptions.dirtStrength, 0.0f, 1.0f);
                }
                ImGui::Checkbox("Dithering", &params.dithering);
                ImGui::Unindent();
            ImGui::Checkbox("FXAA", &params.fxaa);
        }

        if (ImGui::CollapsingHeader("Debug")) {
            DebugRegistry& debug = engine->getDebugRegistry();
            ImGui::Checkbox("Camera at origin",
                    debug.getPropertyAddress<bool>("d.view.camera_at_origin"));
            ImGui::Checkbox("Stable Shadow Map", &params.stableShadowMap);
            ImGui::Checkbox("Light Far uses shadow casters",
                    debug.getPropertyAddress<bool>("d.shadowmap.far_uses_shadowcasters"));
            ImGui::Checkbox("Focus shadow casters",
                    debug.getPropertyAddress<bool>("d.shadowmap.focus_shadowcasters"));
            ImGui::Checkbox("Show checker board",
                    debug.getPropertyAddress<bool>("d.shadowmap.checkerboard"));

            ImGui::SliderFloat("Normal bias", &params.normalBias, 0.0f, 4.0f);
            ImGui::SliderFloat("Constant bias", &params.constantBias, 0.0f, 1.0f);
            ImGui::SliderFloat("Polygon Offset Scale", &params.polygonOffsetSlope, 0.0f, 10.0f);
            ImGui::SliderFloat("Polygon Offset Constant", &params.polygonOffsetConstant, 0.0f, 10.0f);

            bool* lispsm;
            if (debug.getPropertyAddress<bool>("d.shadowmap.lispsm", &lispsm)) {
                ImGui::Checkbox("Enable LiSPSM", lispsm);
                if (*lispsm) {
                    ImGui::SliderFloat("dzn",
                            debug.getPropertyAddress<float>("d.shadowmap.dzn"), 0.0f, 1.0f);
                    ImGui::SliderFloat("dzf",
                            debug.getPropertyAddress<float>("d.shadowmap.dzf"),-1.0f, 0.0f);
                }
            }
        }
    }
    ImGui::End();

    MaterialInstance* materialInstance = updateInstances(params, *engine);

    auto& rcm = engine->getRenderableManager();
    size_t count = 0;
    for (auto renderable : g_meshSet->getRenderables()) {
        auto instance = rcm.getInstance(renderable);
        if (!instance) continue;
        if (!g_singleMode || count == 0) {
            for (size_t i = 0; i < rcm.getPrimitiveCount(instance); i++) {
                rcm.setMaterialInstanceAt(instance, i, materialInstance);
            }
        }
        count++;
        rcm.setCastShadows(instance, params.castShadows);
    }

    if (params.directionalLightEnabled && !params.hasDirectionalLight) {
        g_scene->addEntity(params.light);
        params.hasDirectionalLight = true;
    } else if (!params.directionalLightEnabled && params.hasDirectionalLight) {
        g_scene->remove(params.light);
        params.hasDirectionalLight = false;
    }

    auto* ibl = FilamentApp::get().getIBL();
    if (ibl) {
        ibl->getIndirectLight()->setIntensity(params.iblIntensity);
        ibl->getIndirectLight()->setRotation(
                mat3f::rotation(params.iblRotation, float3{ 0, 1, 0 }));
    }

    auto& lcm = engine->getLightManager();
    auto lightInstance = lcm.getInstance(params.light);
    lcm.setColor(lightInstance, params.lightColor);
    lcm.setIntensity(lightInstance, params.lightIntensity);
    lcm.setDirection(lightInstance, params.lightDirection);
    lcm.setSunAngularRadius(lightInstance, params.sunAngularRadius);
    lcm.setSunHaloSize(lightInstance, params.sunHaloSize);
    lcm.setSunHaloFalloff(lightInstance, params.sunHaloFalloff);

    LightManager::ShadowOptions options = lcm.getShadowOptions(lightInstance);
    options.stable = params.stableShadowMap;
    options.normalBias = params.normalBias;
    options.constantBias = params.constantBias;
    options.polygonOffsetConstant = params.polygonOffsetConstant;
    options.polygonOffsetSlope = params.polygonOffsetSlope;
    options.screenSpaceContactShadows = params.screenSpaceContactShadows;
    options.stepCount = params.stepCount;
    options.maxShadowDistance = params.maxShadowDistance;
    lcm.setShadowOptions(lightInstance, options);

    if (params.spotLightEnabled && !params.hasSpotLight) {
        g_scene->addEntity(params.spotLight);
        params.hasSpotLight = true;
    } else if (!params.spotLightEnabled && params.hasSpotLight) {
        g_scene->remove(params.spotLight);
        params.hasSpotLight = false;
    }
    auto spotLightInstance = lcm.getInstance(params.spotLight);
    auto& tcm = engine->getTransformManager();
    tcm.setTransform(tcm.getInstance(params.spotLight),
            mat4f::translation(params.spotLightPosition));
    lcm.setColor(spotLightInstance, params.spotLightColor);
    lcm.setShadowCaster(spotLightInstance, params.spotLightCastShadows);
    lcm.setIntensity(spotLightInstance, params.spotLightIntensity);
    lcm.setSpotLightCone(spotLightInstance, params.spotLightConeAngle * params.spotLightConeFade,
            params.spotLightConeAngle);
}

static void preRender(filament::Engine*, filament::View* view, filament::Scene*, filament::Renderer*) {
    view->setAntiAliasing(g_params.fxaa ? View::AntiAliasing::FXAA : View::AntiAliasing::NONE);
    view->setToneMapping(g_params.tonemapping ? View::ToneMapping::ACES : View::ToneMapping::LINEAR);
    view->setDithering(g_params.dithering ? View::Dithering::TEMPORAL : View::Dithering::NONE);
    view->setBloomOptions(g_params.bloomOptions);
    view->setFogOptions(g_params.fogOptions);
    view->setSampleCount((uint8_t) (g_params.msaa ? 4 : 1));
    view->setAmbientOcclusion(
            g_params.ssao ? View::AmbientOcclusion::SSAO : View::AmbientOcclusion::NONE);
    view->setAmbientOcclusionOptions(g_params.ssaoOptions);

    Camera& camera = view->getCamera();
    camera.setExposure(g_params.cameraAperture, 1.0f / g_params.cameraSpeed, g_params.cameraISO);
}

int main(int argc, char* argv[]) {
    int option_index = handleCommandLineArgments(argc, argv, &g_config);
    int num_args = argc - option_index;
    if (num_args < 1) {
        printUsage(argv[0]);
        return 1;
    }

    for (int i = option_index; i < argc; i++) {
        utils::Path filename = argv[i];
        if (!filename.exists()) {
            std::cerr << "file " << argv[i] << " not found!" << std::endl;
            return 1;
        }
        g_filenames.push_back(filename);
    }

    g_params.bloomOptions.enabled = true;

    g_config.title = "Material Sandbox";
    FilamentApp& filamentApp = FilamentApp::get();
    filamentApp.run(g_config, setup, cleanup, gui, preRender);
    return 0;
}
