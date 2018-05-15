/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer
    Copyright 2016-2018 Petr Ohlidal & contributors

    For more information, see http://www.rigsofrods.org/

    Rigs of Rods is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3, as
    published by the Free Software Foundation.

    Rigs of Rods is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Rigs of Rods. If not, see <http://www.gnu.org/licenses/>.
*/

#include "GfxActor.h"

#include "ApproxMath.h"
#include "AeroEngine.h"
#include "AirBrake.h"
#include "Beam.h"
#include "beam_t.h"
#include "BeamEngine.h" // EngineSim
#include "Collisions.h"
#include "DustPool.h" // General particle gfx
#include "FlexObj.h"
#include "Flexable.h"
#include "FlexMeshWheel.h"
#include "FlexObj.h"
#include "GlobalEnvironment.h" // TODO: Eliminate!
#include "MovableText.h"
#include "RoRFrameListener.h" // SimController
#include "SkyManager.h"
#include "SoundScriptManager.h"
#include "Utils.h"
#include "TerrainManager.h"
#include "ThreadPool.h"
#include "imgui.h"

#include <OgrePass.h>
#include <OgreRenderWindow.h>
#include <OgreResourceGroupManager.h>
#include <OgreSceneManager.h>
#include <OgreTechnique.h>
#include <OgreTextureUnitState.h>
#include <OgreTextureManager.h>

#include <OgreTextureManager.h>
#include <OgreSceneManager.h>
#include <OgreRenderWindow.h>

RoR::GfxActor::GfxActor(Actor* actor, std::string ogre_resource_group, std::vector<NodeGfx>& gfx_nodes):
    m_actor(actor),
    m_custom_resource_group(ogre_resource_group),
    m_vidcam_state(VideoCamState::VCSTATE_ENABLED_ONLINE),
    m_debug_view(DebugViewType::DEBUGVIEW_NONE),
    m_rods_parent_scenenode(nullptr),
    m_gfx_nodes(gfx_nodes)
{
    // Setup particles
    RoR::GfxScene& dustman = RoR::App::GetSimController()->GetGfxScene();
    m_particles_drip   = dustman.GetDustPool("drip");
    m_particles_misc   = dustman.GetDustPool("dust"); // Dust, water vapour, tyre smoke
    m_particles_splash = dustman.GetDustPool("splash");
    m_particles_ripple = dustman.GetDustPool("ripple");
    m_particles_sparks = dustman.GetDustPool("sparks");
    m_particles_clump  = dustman.GetDustPool("clump");

    m_simbuf.simbuf_nodes.reset(new NodeData[actor->ar_num_nodes]);

    // Attributes
    m_attr.xa_speedo_highest_kph = actor->ar_speedo_max_kph; // TODO: Remove the attribute from Actor altogether ~ only_a_ptr, 05/2018
    m_attr.xa_speedo_use_engine_max_rpm = actor->ar_gui_use_engine_max_rpm; // TODO: ditto
    if (actor->ar_engine != nullptr)
    {
        m_attr.xa_num_gears = actor->ar_engine->getNumGears();
        m_attr.xa_engine_max_rpm = actor->ar_engine->getMaxRPM();
    }
}

RoR::GfxActor::~GfxActor()
{
    // Dispose videocameras
    this->SetVideoCamState(VideoCamState::VCSTATE_DISABLED);
    while (!m_videocameras.empty())
    {
        VideoCamera& vcam = m_videocameras.back();
        Ogre::TextureManager::getSingleton().remove(vcam.vcam_render_tex->getHandle());
        vcam.vcam_render_tex.setNull();
        vcam.vcam_render_target = nullptr; // Invalidated with parent texture
        gEnv->sceneManager->destroyCamera(vcam.vcam_ogre_camera);

        m_videocameras.pop_back();
    }

    // Dispose rods
    if (m_rods_parent_scenenode != nullptr)
    {
        for (Rod& rod: m_rods)
        {
            Ogre::MovableObject* ogre_object = rod.rod_scenenode->getAttachedObject(0);
            rod.rod_scenenode->detachAllObjects();
            gEnv->sceneManager->destroyEntity(static_cast<Ogre::Entity*>(ogre_object));
        }
        m_rods.clear();

        m_rods_parent_scenenode->removeAndDestroyAllChildren();
        gEnv->sceneManager->destroySceneNode(m_rods_parent_scenenode);
        m_rods_parent_scenenode = nullptr;
    }

    // delete meshwheels
    for (size_t i = 0; i < m_wheels.size(); i++)
    {
        if (m_wheels[i].wx_flex_mesh != nullptr)
        {
            delete m_wheels[i].wx_flex_mesh;
        }
        if (m_wheels[i].wx_scenenode != nullptr)
        {
            m_wheels[i].wx_scenenode->removeAndDestroyAllChildren();
            gEnv->sceneManager->destroySceneNode(m_wheels[i].wx_scenenode);
        }
    }

    // delete airbrakes
    for (AirbrakeGfx& abx: m_gfx_airbrakes)
    {
        // scene node
        abx.abx_scenenode->detachAllObjects();
        gEnv->sceneManager->destroySceneNode(abx.abx_scenenode);
        // entity
        gEnv->sceneManager->destroyEntity(abx.abx_entity);
        // mesh
        abx.abx_mesh->unload();
        abx.abx_mesh.setNull();
    }
    m_gfx_airbrakes.clear();

    Ogre::ResourceGroupManager::getSingleton().destroyResourceGroup(m_custom_resource_group);
}

void RoR::GfxActor::AddMaterialFlare(int flareid, Ogre::MaterialPtr m)
{
    RoR::GfxActor::FlareMaterial binding;
    binding.flare_index = flareid;
    binding.mat_instance = m;

    if (m.isNull())
        return;
    Ogre::Technique* tech = m->getTechnique(0);
    if (!tech)
        return;
    Ogre::Pass* p = tech->getPass(0);
    if (!p)
        return;
    // save emissive colour and then set to zero (light disabled by default)
    binding.emissive_color = p->getSelfIllumination();
    p->setSelfIllumination(Ogre::ColourValue::ZERO);

    m_flare_materials.push_back(binding);
}

void RoR::GfxActor::SetMaterialFlareOn(int flare_index, bool state_on)
{
    for (FlareMaterial& entry: m_flare_materials)
    {
        if (entry.flare_index != flare_index)
        {
            continue;
        }

        const int num_techniques = static_cast<int>(entry.mat_instance->getNumTechniques());
        for (int i = 0; i < num_techniques; i++)
        {
            Ogre::Technique* tech = entry.mat_instance->getTechnique(i);
            if (!tech)
                continue;

            if (tech->getSchemeName() == "glow")
            {
                // glowing technique
                // set the ambient value as glow amount
                Ogre::Pass* p = tech->getPass(0);
                if (!p)
                    continue;

                if (state_on)
                {
                    p->setSelfIllumination(entry.emissive_color);
                    p->setAmbient(Ogre::ColourValue::White);
                    p->setDiffuse(Ogre::ColourValue::White);
                }
                else
                {
                    p->setSelfIllumination(Ogre::ColourValue::ZERO);
                    p->setAmbient(Ogre::ColourValue::Black);
                    p->setDiffuse(Ogre::ColourValue::Black);
                }
            }
            else
            {
                // normal technique
                Ogre::Pass* p = tech->getPass(0);
                if (!p)
                    continue;

                Ogre::TextureUnitState* tus = p->getTextureUnitState(0);
                if (!tus)
                    continue;

                if (tus->getNumFrames() < 2)
                    continue;

                int frame = state_on ? 1 : 0;

                tus->setCurrentFrame(frame);

                if (state_on)
                    p->setSelfIllumination(entry.emissive_color);
                else
                    p->setSelfIllumination(Ogre::ColourValue::ZERO);
            }
        } // for each technique
    }
}

void RoR::GfxActor::RegisterCabMaterial(Ogre::MaterialPtr mat, Ogre::MaterialPtr mat_trans)
{
    // Material instances of this actor
    m_cab_mat_visual = mat;
    m_cab_mat_visual_trans = mat_trans;

    if (mat->getTechnique(0)->getNumPasses() == 1)
        return; // No emissive pass -> nothing to do.

    m_cab_mat_template_emissive = mat->clone("CabMaterialEmissive-" + mat->getName(), true, m_custom_resource_group);

    m_cab_mat_template_plain = mat->clone("CabMaterialPlain-" + mat->getName(), true, m_custom_resource_group);
    m_cab_mat_template_plain->getTechnique(0)->removePass(1);
    m_cab_mat_template_plain->compile();
}

void RoR::GfxActor::SetCabLightsActive(bool state_on)
{
    if (m_cab_mat_template_emissive.isNull()) // Both this and '_plain' are only set when emissive pass is present.
        return;

    // NOTE: Updating material in-place like this is probably inefficient,
    //       but in order to maintain all the existing material features working together,
    //       we need to avoid any material swapping on runtime. ~ only_a_ptr, 05/2017
    Ogre::MaterialPtr template_mat = (state_on) ? m_cab_mat_template_emissive : m_cab_mat_template_plain;
    Ogre::Technique* dest_tech = m_cab_mat_visual->getTechnique(0);
    Ogre::Technique* templ_tech = template_mat->getTechnique(0);
    dest_tech->removeAllPasses();
    for (unsigned short i = 0; i < templ_tech->getNumPasses(); ++i)
    {
        Ogre::Pass* templ_pass = templ_tech->getPass(i);
        Ogre::Pass* dest_pass = dest_tech->createPass();
        *dest_pass = *templ_pass; // Copy the pass! Reference: http://www.ogre3d.org/forums/viewtopic.php?f=2&t=83453
    }
    m_cab_mat_visual->compile();
}

void RoR::GfxActor::SetVideoCamState(VideoCamState state)
{
    if (state == m_vidcam_state)
    {
        return; // Nothing to do.
    }

    const bool enable = (state == VideoCamState::VCSTATE_ENABLED_ONLINE);
    for (VideoCamera vidcam: m_videocameras)
    {
        if (vidcam.vcam_render_target != nullptr)
        {
            vidcam.vcam_render_target->setActive(enable);
            if (enable)
                vidcam.vcam_material->getTechnique(0)->getPass(0)->getTextureUnitState(0)->setTextureName(vidcam.vcam_render_tex->getName());
            else
                vidcam.vcam_material->getTechnique(0)->getPass(0)->getTextureUnitState(0)->setTextureName(vidcam.vcam_off_tex_name);
            continue;
        }

        if (vidcam.vcam_render_window != nullptr)
        {
            vidcam.vcam_render_window->setActive(enable);
        }
    }
    m_vidcam_state = state;
}

RoR::GfxActor::VideoCamera::VideoCamera():
    vcam_type(VideoCamType::VCTYPE_INVALID), // VideoCamType
    vcam_node_center(node_t::INVALID_IDX),
    vcam_node_dir_y(node_t::INVALID_IDX),
    vcam_node_dir_z(node_t::INVALID_IDX),
    vcam_node_alt_pos(node_t::INVALID_IDX),
    vcam_node_lookat(node_t::INVALID_IDX),
    vcam_pos_offset(Ogre::Vector3::ZERO), // Ogre::Vector3
    vcam_ogre_camera(nullptr),            // Ogre::Camera*
    vcam_render_target(nullptr),          // Ogre::RenderTexture*
    vcam_debug_node(nullptr),             // Ogre::SceneNode*
    vcam_render_window(nullptr),          // Ogre::RenderWindow*
    vcam_prop_scenenode(nullptr)          // Ogre::SceneNode*
{}

RoR::GfxActor::NodeGfx::NodeGfx(uint16_t node_idx):
    nx_node_idx(node_idx),
    nx_wet_time_sec(-1.f), // node is dry
    nx_no_particles(false),
    nx_may_get_wet(false),
    nx_is_hot(false),
    nx_no_sparks(true),
    nx_under_water_prev(false)
{}

void RoR::GfxActor::UpdateVideoCameras(float dt_sec)
{
    if (m_vidcam_state != VideoCamState::VCSTATE_ENABLED_ONLINE)
        return;

    for (GfxActor::VideoCamera vidcam: m_videocameras)
    {
#ifdef USE_CAELUM
        // caelum needs to know that we changed the cameras
        SkyManager* sky = App::GetSimTerrain()->getSkyManager();
        if ((sky != nullptr) && (RoR::App::app_state.GetActive() == RoR::AppState::SIMULATION))
        {
            sky->NotifySkyCameraChanged(vidcam.vcam_ogre_camera);
        }
#endif // USE_CAELUM

        if ((vidcam.vcam_type == VideoCamType::VCTYPE_MIRROR_PROP_LEFT)
            || (vidcam.vcam_type == VideoCamType::VCTYPE_MIRROR_PROP_RIGHT))
        {
            // Mirror prop - special processing.
            float mirror_angle = 0.f;
            Ogre::Vector3 offset(Ogre::Vector3::ZERO);
            if (vidcam.vcam_type == VideoCamType::VCTYPE_MIRROR_PROP_LEFT)
            {
                mirror_angle = m_actor->ar_left_mirror_angle;
                offset = Ogre::Vector3(0.07f, -0.22f, 0);
            }
            else
            {
                mirror_angle = m_actor->ar_right_mirror_angle;
                offset = Ogre::Vector3(0.07f, +0.22f, 0);
            }

            Ogre::Vector3 normal = vidcam.vcam_prop_scenenode->getOrientation()
                    * Ogre::Vector3(cos(mirror_angle), sin(mirror_angle), 0.0f);
            Ogre::Vector3 center = vidcam.vcam_prop_scenenode->getPosition()
                    + vidcam.vcam_prop_scenenode->getOrientation() * offset;
            Ogre::Radian roll = Ogre::Degree(360)
                - Ogre::Radian(asin(m_actor->getDirection().dotProduct(Ogre::Vector3::UNIT_Y)));

            Ogre::Plane plane = Ogre::Plane(normal, center);
            Ogre::Vector3 project = plane.projectVector(gEnv->mainCamera->getPosition() - center);

            vidcam.vcam_ogre_camera->setPosition(center);
            vidcam.vcam_ogre_camera->lookAt(gEnv->mainCamera->getPosition() - 2.0f * project);
            vidcam.vcam_ogre_camera->roll(roll);

            continue; // Done processing mirror prop.
        }

        // update the texture now, otherwise shuttering
        if (vidcam.vcam_render_target != nullptr)
            vidcam.vcam_render_target->update();

        if (vidcam.vcam_render_window != nullptr)
            vidcam.vcam_render_window->update();

        // get the normal of the camera plane now
        GfxActor::NodeData* node_buf = m_simbuf.simbuf_nodes.get();
        const Ogre::Vector3 abs_pos_center = node_buf[vidcam.vcam_node_center].AbsPosition;
        const Ogre::Vector3 abs_pos_z = node_buf[vidcam.vcam_node_dir_z].AbsPosition;
        const Ogre::Vector3 abs_pos_y = node_buf[vidcam.vcam_node_dir_y].AbsPosition;
        Ogre::Vector3 normal = (-(abs_pos_center - abs_pos_z)).crossProduct(-(abs_pos_center - abs_pos_y));
        normal.normalise();

        // add user set offset
        Ogre::Vector3 pos = node_buf[vidcam.vcam_node_alt_pos].AbsPosition +
            (vidcam.vcam_pos_offset.x * normal) +
            (vidcam.vcam_pos_offset.y * (abs_pos_center - abs_pos_y)) +
            (vidcam.vcam_pos_offset.z * (abs_pos_center - abs_pos_z));

        //avoid the camera roll
        // camup orientates to frustrum of world by default -> rotating the cam related to trucks yaw, lets bind cam rotation videocamera base (nref,ny,nz) as frustum
        // could this be done faster&better with a plane setFrustumExtents ?
        Ogre::Vector3 frustumUP = abs_pos_center - abs_pos_y;
        frustumUP.normalise();
        vidcam.vcam_ogre_camera->setFixedYawAxis(true, frustumUP);

        if (vidcam.vcam_type == GfxActor::VideoCamType::VCTYPE_MIRROR)
        {
            //rotate the normal of the mirror by user rotation setting so it reflects correct
            normal = vidcam.vcam_rotation * normal;
            // merge camera direction and reflect it on our plane
            vidcam.vcam_ogre_camera->setDirection((pos - gEnv->mainCamera->getPosition()).reflect(normal));
        }
        else if (vidcam.vcam_type == GfxActor::VideoCamType::VCTYPE_VIDEOCAM)
        {
            // rotate the camera according to the nodes orientation and user rotation
            Ogre::Vector3 refx = abs_pos_z - abs_pos_center;
            refx.normalise();
            Ogre::Vector3 refy = abs_pos_center - abs_pos_y;
            refy.normalise();
            Ogre::Quaternion rot = Ogre::Quaternion(-refx, -refy, -normal);
            vidcam.vcam_ogre_camera->setOrientation(rot * vidcam.vcam_rotation); // rotate the camera orientation towards the calculated cam direction plus user rotation
        }
        else if (vidcam.vcam_type == GfxActor::VideoCamType::VCTYPE_TRACKING_VIDEOCAM)
        {
            normal = node_buf[vidcam.vcam_node_lookat].AbsPosition - pos;
            normal.normalise();
            Ogre::Vector3 refx = abs_pos_z - abs_pos_center;
            refx.normalise();
            // why does this flip ~2-3� around zero orientation and only with trackercam. back to slower crossproduct calc, a bit slower but better .. sigh
            // Ogre::Vector3 refy = abs_pos_center - abs_pos_y;
            Ogre::Vector3 refy = refx.crossProduct(normal);
            refy.normalise();
            Ogre::Quaternion rot = Ogre::Quaternion(-refx, -refy, -normal);
            vidcam.vcam_ogre_camera->setOrientation(rot * vidcam.vcam_rotation); // rotate the camera orientation towards the calculated cam direction plus user rotation
        }

        if (vidcam.vcam_debug_node != nullptr)
        {
            vidcam.vcam_debug_node->setPosition(pos);
            vidcam.vcam_debug_node->setOrientation(vidcam.vcam_ogre_camera->getOrientation());
        }

        // set the new position
        vidcam.vcam_ogre_camera->setPosition(pos);
    }
}

void RoR::GfxActor::UpdateParticles(float dt_sec)
{
    const bool use_skidmarks = m_actor->GetUseSkidmarks();
    float water_height = 0.f; // Unused if terrain has no water
    if (App::GetSimTerrain()->getWater() != nullptr)
    {
        water_height = App::GetSimTerrain()->getWater()->GetStaticWaterHeight();
    }

    for (NodeGfx& nfx: m_gfx_nodes)
    {
        const node_t& n = m_actor->ar_nodes[nfx.nx_node_idx];

        // 'Wet' effects - water dripping and vapour
        if (nfx.nx_may_get_wet && !nfx.nx_no_particles)
        {
            // Getting out of water?
            if (!n.nd_under_water && nfx.nx_under_water_prev)
            {
                nfx.nx_wet_time_sec = 0.f;
            }

            // Dripping water?
            if (nfx.nx_wet_time_sec != -1)
            {
                nfx.nx_wet_time_sec += dt_sec;
                if (nfx.nx_wet_time_sec > 5.f) // Dries off in 5 sec
                {
                    nfx.nx_wet_time_sec = -1.f;
                }
                else if (nfx.nx_may_get_wet)
                {
                    if (m_particles_drip != nullptr)
                    {
                        m_particles_drip->allocDrip(n.AbsPosition, n.Velocity, nfx.nx_wet_time_sec); // Dripping water particles
                    }
                    if (nfx.nx_is_hot && m_particles_misc != nullptr)
                    {
                        m_particles_misc->allocVapour(n.AbsPosition, n.Velocity, nfx.nx_wet_time_sec); // Water vapour particles
                    }
                }
            }
        }

        // Water splash and ripple
        if (n.nd_under_water && !nfx.nx_no_particles)
        {
            if ((water_height - n.AbsPosition.y < 0.2f) && (n.Velocity.squaredLength() > 4.f))
            {
                if (m_particles_splash)
                {
                    m_particles_splash->allocSplash(n.AbsPosition, n.Velocity);
                }
                if (m_particles_ripple)
                {
                    m_particles_ripple->allocRipple(n.AbsPosition, n.Velocity);     
                }
            }
        }

        // Ground collision (dust, sparks, tyre smoke, clumps...)
        if (n.nd_collision_gm != nullptr && !nfx.nx_no_particles)
        {
            switch (n.nd_collision_gm->fx_type)
            {
            case Collisions::FX_DUSTY:
                if (m_particles_misc != nullptr)
                {
                    m_particles_misc->malloc(n.AbsPosition, n.Velocity / 2.0, n.nd_collision_gm->fx_colour);
                }
                break;

            case Collisions::FX_CLUMPY:
                if (m_particles_clump != nullptr && n.Velocity.squaredLength() > 1.f)
                {
                    m_particles_clump->allocClump(n.AbsPosition, n.Velocity / 2.0, n.nd_collision_gm->fx_colour);
                }
                break;

            case Collisions::FX_HARD:
                if (n.iswheel != 0) // This is a wheel => skidmarks and tyre smoke
                {
                    const float SKID_THRESHOLD = 10.f;
                    if (n.nd_collision_slip > SKID_THRESHOLD)
                    {
                        SOUND_MODULATE(m_actor, SS_MOD_SCREETCH, (n.nd_collision_slip - SKID_THRESHOLD) / SKID_THRESHOLD);
                        SOUND_PLAY_ONCE(m_actor, SS_TRIG_SCREETCH);
                        
                        if (m_particles_misc != nullptr)
                        {
                            m_particles_misc->allocSmoke(n.AbsPosition, n.Velocity);
                        }

                        if (use_skidmarks)
                        {
                            // TODO: Ported as-is from 'calcNodes()', doesn't make sense here
                            //       -> we can update the skidmark GFX directly. ~only_a_ptr, 04/2018
                            m_actor->ar_wheels[n.wheelid].isSkiding = true;
                            if (!(n.iswheel % 2))
                            {
                                m_actor->ar_wheels[n.wheelid].lastContactInner = n.AbsPosition;
                            }
                            else
                            {
                                m_actor->ar_wheels[n.wheelid].lastContactOuter = n.AbsPosition;
                            }

                            m_actor->ar_wheels[n.wheelid].lastContactType = (n.iswheel % 2);
                            m_actor->ar_wheels[n.wheelid].lastSlip = n.nd_collision_slip;
                            m_actor->ar_wheels[n.wheelid].lastGroundModel = n.nd_collision_gm;
                        }
                    }
                    else
                    {
                        if (use_skidmarks)
                        {
                            // TODO: Ported as-is from 'calcNodes()', doesn't make sense here
                            //       -> we can update the skidmark GFX directly. ~only_a_ptr, 04/2018
                            m_actor->ar_wheels[n.wheelid].isSkiding = false;
                        }
                    }
                }
                else // Not a wheel => sparks
                {
                    if ((!nfx.nx_no_sparks) && (n.nd_collision_slip > 1.f) && (m_particles_sparks != nullptr))
                    {
                        m_particles_sparks->allocSparks(n.AbsPosition, n.Velocity);
                    }
                }
                break;

            default:;
            }
        }

        nfx.nx_under_water_prev = n.nd_under_water;
    }
}

const ImU32 BEAM_COLOR               (0xff556633); // All colors are in ABGR format (alpha, blue, green, red)
const float BEAM_THICKNESS           (1.2f);
const ImU32 BEAM_BROKEN_COLOR        (0xff4466dd);
const float BEAM_BROKEN_THICKNESS    (1.8f);
const ImU32 BEAM_HYDRO_COLOR         (0xff55a3e0);
const float BEAM_HYDRO_THICKNESS     (1.4f);
const ImU32 BEAM_STRENGTH_TEXT_COLOR (0xffcfd0cc);
const ImU32 BEAM_STRESS_TEXT_COLOR   (0xff58bbfc);
const ImU32 BEAM_COMPRESS_TEXT_COLOR (0xffccbf3c);
// TODO: commands cannot be distinguished on runtime

const ImU32 NODE_COLOR               (0xff44ddff);
const float NODE_RADIUS              (2.f);
const ImU32 NODE_TEXT_COLOR          (0xffcccccf); // node ID text color
const ImU32 NODE_MASS_TEXT_COLOR     (0xff77bb66);
const ImU32 NODE_IMMOVABLE_COLOR     (0xff0033ff);
const float NODE_IMMOVABLE_RADIUS    (2.8f);

void RoR::GfxActor::UpdateDebugView()
{
    if (m_debug_view == DebugViewType::DEBUGVIEW_NONE)
    {
        return; // Nothing to do
    }

    // Original 'debugVisuals' and their replacements ~only_a_ptr, 06/2017
    // -------------------------------------------------------------------
    // [1] node-numbers:  -------  Draws node numbers (black letters with white outline), generated nodes (wheels...) get fake sequentially assigned numbers.
    //                             Replacement: DEBUGVIEW_NODES; Note: real node_t::id value is displayed - generated nodes show "-1"
    // [2] beam-numbers:  -------  Draws beam numbers (sequentially assigned) as black (thick+distorted) text - almost unreadable, barely useful IMO.
    //                             Not preserved
    // [3] node-and-beam-numbers:  [1] + [2] combined
    //                             Not preserved
    // [4] node-mass:  ----------  Shows mass in same style as [1]
    //                             Replacement: Extra info in DEBUGVIEW_NODES with different text color, like "33 (3.3Kg)"
    // [5] node-locked:  --------  Shows text "unlocked"/"locked" in same style as [1]
    //                             replacement: colored circles around nodes showing PRELOCK and LOCKED states (not shown when ulocked) - used in all DEBUGVIEW_* modes
    // [6] beam-compression:  ---  A number shown per beam, same style as [2] - unreadable. Logic:
    //                              // float stress_ratio = beams[it->id].stress / beams[it->id].minmaxposnegstress;
    //                              // float color_scale = std::abs(stress_ratio);
    //                              // color_scale = std::min(color_scale, 1.0f);
    //                              // int scale = (int)(color_scale * 100);
    //                              // it->txt->setCaption(TOSTRING(scale));
    //                             Replacement: DEBUGVIEW_BEAMS -- modified logic, simplified, specific text color
    // [7] beam-broken  ---------  Shows "BROKEN" label for broken beams (`beam_t::broken` flag) in same style as [2]
    //                             Replacement - special coloring/display in DEBUGVIEW_* modes.
    // [8] beam-stress  ---------  Shows value of `beam_t::stress` in style of [2]
    //                             Replacement: DEBUGVIEW_BEAMS + specific text color
    // [9] beam-hydro  ----------  Shows a per-hydro number in style of [2], formula: `(beams[it->id].L / beams[it->id].Lhydro) * 100`
    //                             Replacement: DEBUGVIEW_BEAMS + specific text color
    // [9] beam-commands  -------  Shows a per-beam number in style of [2], formula: `(beams[it->id].L / beams[it->id].commandLong) * 100`
    //                             Not preserved - there's no way to distinguish commands on runtime and the number makes no sense for all beams. TODO: make commands distinguishable on runtime!

    // Var
    ImVec2 screen_size = ImGui::GetIO().DisplaySize;
    World2ScreenConverter world2screen(
        gEnv->mainCamera->getViewMatrix(true), gEnv->mainCamera->getProjectionMatrix(), Ogre::Vector2(screen_size.x, screen_size.y));

    // Dummy fullscreen window to draw to
    int window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar| ImGuiWindowFlags_NoInputs 
                     | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("RoR-SoftBodyView", NULL, screen_size, 0, window_flags);
    ImDrawList* drawlist = ImGui::GetWindowDrawList();
    ImGui::End();

    // Skeleton display. NOTE: Order matters, it determines Z-ordering on render
    if ((m_debug_view == DebugViewType::DEBUGVIEW_SKELETON) ||
        (m_debug_view == DebugViewType::DEBUGVIEW_NODES) ||
        (m_debug_view == DebugViewType::DEBUGVIEW_BEAMS))
    {
        // Beams
        const beam_t* beams = m_actor->ar_beams;
        const size_t num_beams = static_cast<size_t>(m_actor->ar_num_beams);
        for (size_t i = 0; i < num_beams; ++i)
        {
            Ogre::Vector3 pos1 = world2screen.Convert(beams[i].p1->AbsPosition);
            Ogre::Vector3 pos2 = world2screen.Convert(beams[i].p2->AbsPosition);

            // Original coloring logic for "skeletonview":
            // -------------------------------------------
            // // float stress_ratio = beams[i].stress / beams[i].minmaxposnegstress;
            // // float color_scale = std::abs(stress_ratio);
            // // color_scale = std::min(color_scale, 1.0f);
            // // 
            // // if (stress_ratio <= 0)
            // //     color = ColourValue(0.2f, 1.0f - color_scale, color_scale, 0.8f);
            // // else
            // //     color = ColourValue(color_scale, 1.0f - color_scale, 0.2f, 0.8f);

            if ((pos1.z < 0.f) && (pos2.z < 0.f))
            {
                ImVec2 pos1xy(pos1.x, pos1.y);
                ImVec2 pos2xy(pos2.x, pos2.y);

                if (beams[i].bm_broken)
                {
                    drawlist->AddLine(pos1xy, pos2xy, BEAM_BROKEN_COLOR, BEAM_BROKEN_THICKNESS);
                }
                else if (beams[i].bm_type == BEAM_HYDRO)
                {
                    drawlist->AddLine(pos1xy, pos2xy, BEAM_HYDRO_COLOR, BEAM_HYDRO_THICKNESS);
                }
                else
                {
                    drawlist->AddLine(pos1xy, pos2xy, BEAM_COLOR, BEAM_THICKNESS);
                }
            }
        }

        // Nodes
        const node_t* nodes = m_actor->ar_nodes;
        const size_t num_nodes = static_cast<size_t>(m_actor->ar_num_nodes);
        for (size_t i = 0; i < num_nodes; ++i)
        {
            Ogre::Vector3 pos_xyz = world2screen.Convert(nodes[i].AbsPosition);

            if (pos_xyz.z < 0.f)
            {
                ImVec2 pos(pos_xyz.x, pos_xyz.y);
                if (nodes[i].nd_immovable)
                {
                    drawlist->AddCircleFilled(pos, NODE_IMMOVABLE_RADIUS, NODE_IMMOVABLE_COLOR);
                }
                else
                {
                    drawlist->AddCircleFilled(pos, NODE_RADIUS, NODE_COLOR);
                }
            }
        }

        // Node info; drawn after nodes to have higher Z-order
        if ((m_debug_view == DebugViewType::DEBUGVIEW_NODES) || (m_debug_view == DebugViewType::DEBUGVIEW_BEAMS))
        {
            for (size_t i = 0; i < num_nodes; ++i)
            {
                Ogre::Vector3 pos = world2screen.Convert(nodes[i].AbsPosition);

                if (pos.z < 0.f)
                {
                    ImVec2 pos_xy(pos.x, pos.y);
                    Str<25> id_buf;
                    id_buf << nodes[i].id;
                    drawlist->AddText(pos_xy, NODE_TEXT_COLOR, id_buf.ToCStr());

                    if (m_debug_view != DebugViewType::DEBUGVIEW_BEAMS)
                    {
                        char mass_buf[50];
                        snprintf(mass_buf, 50, "|%.1fKg", nodes[i].mass);
                        ImVec2 offset = ImGui::CalcTextSize(id_buf.ToCStr());
                        drawlist->AddText(ImVec2(pos.x + offset.x, pos.y), NODE_MASS_TEXT_COLOR, mass_buf);
                    }
                }
            }
        }

        // Beam-info: drawn after beams to have higher Z-order
        if (m_debug_view == DebugViewType::DEBUGVIEW_BEAMS)
        {
            for (size_t i = 0; i < num_beams; ++i)
            {
                // Position
                Ogre::Vector3 world_pos = (beams[i].p1->AbsPosition + beams[i].p2->AbsPosition) / 2.f;
                Ogre::Vector3 pos_xyz = world2screen.Convert(world_pos);
                if (pos_xyz.z >= 0.f)
                {
                    continue; // Behind the camera
                }
                ImVec2 pos(pos_xyz.x, pos_xyz.y);

                // Strength is usually in thousands or millions - we shorten it.
                const size_t BUF_LEN = 50;
                char buf[BUF_LEN];
                if (beams[i].strength >= 1000000.f)
                {
                    snprintf(buf, BUF_LEN, "%.2fM", (beams[i].strength / 1000000.f));
                }
                else if (beams[i].strength >= 1000.f)
                {
                    snprintf(buf, BUF_LEN, "%.1fK", (beams[i].strength / 1000.f));
                }
                else
                {
                    snprintf(buf, BUF_LEN, "%.2f", beams[i].strength);
                }
                drawlist->AddText(pos, BEAM_STRENGTH_TEXT_COLOR, buf);
                const ImVec2 stren_text_size = ImGui::CalcTextSize(buf);

                // Compression
                snprintf(buf, BUF_LEN, "|%.2f",  std::abs(beams[i].stress / beams[i].minmaxposnegstress)); // NOTE: New formula (simplified); the old one didn't make sense to me ~ only_a_ptr, 06/2017
                drawlist->AddText(ImVec2(pos.x + stren_text_size.x, pos.y), BEAM_COMPRESS_TEXT_COLOR, buf);

                // Stress
                snprintf(buf, BUF_LEN, "%.1f", beams[i].stress);
                ImVec2 stress_text_pos(pos.x, pos.y + stren_text_size.y);
                drawlist->AddText(stress_text_pos, BEAM_STRESS_TEXT_COLOR, buf);

                // TODO: Hydro stress
            }
        }
    }
}

void RoR::GfxActor::CycleDebugViews()
{
    switch (m_debug_view)
    {
    case DebugViewType::DEBUGVIEW_NONE:     m_debug_view = DebugViewType::DEBUGVIEW_SKELETON; break;
    case DebugViewType::DEBUGVIEW_SKELETON: m_debug_view = DebugViewType::DEBUGVIEW_NODES;    break;
    case DebugViewType::DEBUGVIEW_NODES:    m_debug_view = DebugViewType::DEBUGVIEW_BEAMS;    break;
    case DebugViewType::DEBUGVIEW_BEAMS:    m_debug_view = DebugViewType::DEBUGVIEW_NONE;     break;
    default:;
    }
}

void RoR::GfxActor::AddRod(int beam_index,  int node1_index, int node2_index, const char* material_name, bool visible, float diameter_meters)
{
    try
    {
        Str<100> entity_name;
        entity_name << "rod" << beam_index << "@actor" << m_actor->ar_instance_id;
        Ogre::Entity* entity = gEnv->sceneManager->createEntity(entity_name.ToCStr(), "beam.mesh");
        entity->setMaterialName(material_name);

        if (m_rods_parent_scenenode == nullptr)
        {
            m_rods_parent_scenenode = gEnv->sceneManager->getRootSceneNode()->createChildSceneNode();
        }

        Rod rod;
        rod.rod_scenenode = m_rods_parent_scenenode->createChildSceneNode();
        rod.rod_scenenode->attachObject(entity);
        rod.rod_scenenode->setVisible(visible, /*cascade=*/ false);

        rod.rod_scenenode->setScale(diameter_meters, -1, diameter_meters);
        rod.rod_diameter_mm = uint16_t(diameter_meters * 1000.f);

        rod.rod_beam_index = static_cast<uint16_t>(beam_index);
        rod.rod_node1 = static_cast<uint16_t>(node1_index);
        rod.rod_node2 = static_cast<uint16_t>(node2_index);

        m_rods.push_back(rod);
    }
    catch (Ogre::Exception& e)
    {
        LogFormat("[RoR|Gfx] Failed to create visuals for beam %d, message: %s", beam_index, e.getFullDescription().c_str());
    }
}

void RoR::GfxActor::UpdateRods()
{
    // TODO: Apply visibility updates from a queue (to be implemented!)
    // fulltext-label: QUEUE_VIS_CHANGE

    for (Rod& rod: m_rods)
    {
        Ogre::Vector3 pos1 = m_actor->ar_nodes[rod.rod_node1].AbsPosition;
        Ogre::Vector3 pos2 = m_actor->ar_nodes[rod.rod_node2].AbsPosition;

        // Classic method
        float beam_diameter = static_cast<float>(rod.rod_diameter_mm) * 0.001;
        float beam_length = pos1.distance(pos2);

        rod.rod_scenenode->setPosition(pos1.midPoint(pos2));
        rod.rod_scenenode->setScale(beam_diameter, beam_length, beam_diameter);
        rod.rod_scenenode->setOrientation(GfxActor::SpecialGetRotationTo(Ogre::Vector3::UNIT_Y, (pos1 - pos2)));
    }
}

Ogre::Quaternion RoR::GfxActor::SpecialGetRotationTo(const Ogre::Vector3& src, const Ogre::Vector3& dest)
{
    // Based on Stan Melax's article in Game Programming Gems
    Ogre::Quaternion q;
    // Copy, since cannot modify local
    Ogre::Vector3 v0 = src;
    Ogre::Vector3 v1 = dest;
    v0.normalise();
    v1.normalise();

    // NB if the crossProduct approaches zero, we get unstable because ANY axis will do
    // when v0 == -v1
    Ogre::Real d = v0.dotProduct(v1);
    // If dot == 1, vectors are the same
    if (d >= 1.0f)
    {
        return Ogre::Quaternion::IDENTITY;
    }
    if (d < (1e-6f - 1.0f))
    {
        // Generate an axis
        Ogre::Vector3 axis = Ogre::Vector3::UNIT_X.crossProduct(src);
        if (axis.isZeroLength()) // pick another if colinear
            axis = Ogre::Vector3::UNIT_Y.crossProduct(src);
        axis.normalise();
        q.FromAngleAxis(Ogre::Radian(Ogre::Math::PI), axis);
    }
    else
    {
        Ogre::Real s = fast_sqrt((1 + d) * 2);
        if (s == 0)
            return Ogre::Quaternion::IDENTITY;

        Ogre::Vector3 c = v0.crossProduct(v1);
        Ogre::Real invs = 1 / s;

        q.x = c.x * invs;
        q.y = c.y * invs;
        q.z = c.z * invs;
        q.w = s * 0.5;
    }
    return q;
}

void RoR::GfxActor::ScaleActor(float ratio)
{
    for (Rod& rod: m_rods)
    {
        float diameter2 = static_cast<float>(rod.rod_diameter_mm) * (ratio*1000.f);
        rod.rod_diameter_mm = static_cast<uint16_t>(diameter2);
    }
}

void RoR::GfxActor::SetRodsVisible(bool visible)
{
    if (m_rods_parent_scenenode == nullptr)
    {
        return; // Vehicle has no visual softbody beams -> nothing to do.
    }

    // NOTE: We don't use Ogre::SceneNode::setVisible() for performance reasons:
    //       1. That function traverses all attached Entities and updates their visibility - too much overhead
    //       2. For OGRE up to 1.9 (I don't know about 1.10+) OGRE developers recommended to detach rather than hide.
    //       ~ only_a_ptr, 12/2017
    if (visible && !m_rods_parent_scenenode->isInSceneGraph())
    {
        gEnv->sceneManager->getRootSceneNode()->addChild(m_rods_parent_scenenode);
    }
    else if (!visible && m_rods_parent_scenenode->isInSceneGraph())
    {
        gEnv->sceneManager->getRootSceneNode()->removeChild(m_rods_parent_scenenode);
    }
}

void RoR::GfxActor::UpdateSimDataBuffer()
{
    m_simbuf.simbuf_live_local = (m_actor->ar_sim_state == Actor::SimState::LOCAL_SIMULATED);
    m_simbuf.simbuf_pos = m_actor->getPosition();
    m_simbuf.simbuf_heading_angle = m_actor->getHeadingDirectionAngle();
    m_simbuf.simbuf_tyre_pressure = m_actor->GetTyrePressure();
    m_simbuf.simbuf_aabb = m_actor->ar_bounding_box;
    m_simbuf.simbuf_wheel_speed = m_actor->ar_wheel_speed;
    if (m_simbuf.simbuf_net_username != m_actor->m_net_username)
    {
        m_simbuf.simbuf_net_username = m_actor->m_net_username;
    }

    // nodes
    const int num_nodes = m_actor->ar_num_nodes;
    for (int i = 0; i < num_nodes; ++i)
    {
        m_simbuf.simbuf_nodes.get()[i].AbsPosition = m_actor->ar_nodes[i].AbsPosition;
    }

    // airbrakes
    const int num_airbrakes = m_actor->ar_num_airbrakes;
    m_simbuf.simbuf_airbrakes.clear();
    for (int i=0; i< num_airbrakes; ++i)
    {
        m_simbuf.simbuf_airbrakes.push_back(m_actor->ar_airbrakes[i]->ratio);
    }

    // Engine (+drivetrain)
    if (m_actor->ar_engine != nullptr)
    {
        m_simbuf.simbuf_gear            = m_actor->ar_engine->GetGear();
        m_simbuf.simbuf_autoshift       = m_actor->ar_engine->getAutoShift();
        m_simbuf.simbuf_engine_rpm      = m_actor->ar_engine->GetEngineRpm();
    }
}

bool RoR::GfxActor::IsActorLive() const
{
    return (m_actor->ar_sim_state < Actor::SimState::LOCAL_SLEEPING);
}

void RoR::GfxActor::UpdateCabMesh()
{
    // TODO: Hacky, requires 'friend' access to `Actor` -> move the visuals to GfxActor!
    if ((m_actor->m_cab_entity != nullptr) && (m_actor->m_cab_mesh != nullptr))
    {
        m_actor->m_cab_scene_node->setPosition(m_actor->m_cab_mesh->UpdateFlexObj());
    }
}

void RoR::GfxActor::SetWheelVisuals(uint16_t index, WheelGfx wheel_gfx)
{
    if (m_wheels.size() <= index)
    {
        m_wheels.resize(index + 1);
    }
    m_wheels[index] = wheel_gfx;
}

void RoR::GfxActor::UpdateWheelVisuals()
{
    m_flexwheel_tasks.clear();
    if (gEnv->threadPool)
    {
        for (WheelGfx& w: m_wheels)
        {
            if ((w.wx_scenenode != nullptr) && w.wx_flex_mesh->flexitPrepare())
            {
                auto func = std::function<void()>([this, w]()
                    {
                        w.wx_flex_mesh->flexitCompute();
                    });
                auto task_handle = gEnv->threadPool->RunTask(func);
                m_flexwheel_tasks.push_back(task_handle);
            }
        }
    }
    else
    {
        for (WheelGfx& w: m_wheels)
        {
            if ((w.wx_scenenode != nullptr) && w.wx_flex_mesh->flexitPrepare())
            {
                w.wx_flex_mesh->flexitCompute();
                w.wx_scenenode->setPosition(w.wx_flex_mesh->flexitFinal());
            }
        }
    }
}

void RoR::GfxActor::FinishWheelUpdates()
{
    if (gEnv->threadPool)
    {
        for (auto& task: m_flexwheel_tasks)
        {
            task->join();
        }
        for (WheelGfx& w: m_wheels)
        {
            if (w.wx_scenenode != nullptr)
            {
                w.wx_scenenode->setPosition(w.wx_flex_mesh->flexitFinal());
            }
        }
    }
}

void RoR::GfxActor::SetWheelsVisible(bool value)
{
    for (WheelGfx& w: m_wheels)
    {
        if (w.wx_scenenode != nullptr)
        {
            w.wx_scenenode->setVisible(value);
        }
        if (w.wx_flex_mesh != nullptr)
        {
            w.wx_flex_mesh->setVisible(value);
            if (w.wx_is_meshwheel)
            {
                Ogre::Entity* e = ((FlexMeshWheel*)(w.wx_flex_mesh))->getRimEntity();
                if (e != nullptr)
                {
                    e->setVisible(false);
                }
            }
        }
    }
}


int RoR::GfxActor::GetActorId          () const { return m_actor->ar_instance_id; }
int RoR::GfxActor::GetActorDriveable   () const { return m_actor->ar_driveable; }

void RoR::GfxActor::RegisterAirbrakes()
{
    // TODO: Quick hacky setup with `friend` access - we rely on old init code in RigSpawner/Airbrake.
    for (int i=0; i< m_actor->ar_num_airbrakes; ++i)
    {
        Airbrake* ab = m_actor->ar_airbrakes[i];
        AirbrakeGfx abx;
        // entity
        abx.abx_entity = ab->ec;
        ab->ec = nullptr;
        // mesh
        abx.abx_mesh = ab->msh;
        ab->msh.setNull();
        // scenenode
        abx.abx_scenenode = ab->snode;
        ab->snode = nullptr;
        // offset
        abx.abx_offset = ab->offset;
        ab->offset = Ogre::Vector3::ZERO;
        // Nodes - just copy
        abx.abx_ref_node = ab->noderef->pos;
        abx.abx_x_node = ab->nodex->pos;
        abx.abx_y_node = ab->nodey->pos;

        m_gfx_airbrakes.push_back(abx);
    }
}

void RoR::GfxActor::UpdateAirbrakes()
{
    const size_t num_airbrakes = m_gfx_airbrakes.size();
    NodeData* nodes = m_simbuf.simbuf_nodes.get();
    for (size_t i=0; i<num_airbrakes; ++i)
    {
        AirbrakeGfx abx = m_gfx_airbrakes[i];
        const float ratio = m_simbuf.simbuf_airbrakes[i];
        const float maxangle = m_actor->ar_airbrakes[i]->maxangle; // Friend access
        Ogre::Vector3 ref_node_pos = nodes[m_gfx_airbrakes[i].abx_ref_node].AbsPosition;
        Ogre::Vector3 x_node_pos   = nodes[m_gfx_airbrakes[i].abx_x_node].AbsPosition;
        Ogre::Vector3 y_node_pos   = nodes[m_gfx_airbrakes[i].abx_y_node].AbsPosition;

        // -- Ported from `AirBrake::updatePosition()` --
        Ogre::Vector3 normal = (y_node_pos - ref_node_pos).crossProduct(x_node_pos - ref_node_pos);
        normal.normalise();
        //position
        Ogre::Vector3 mposition = ref_node_pos + abx.abx_offset.x * (x_node_pos - ref_node_pos) + abx.abx_offset.y * (y_node_pos - ref_node_pos);
        abx.abx_scenenode->setPosition(mposition + normal * abx.abx_offset.z);
        //orientation
        Ogre::Vector3 refx = x_node_pos - ref_node_pos;
        refx.normalise();
        Ogre::Vector3 refy = refx.crossProduct(normal);
        Ogre::Quaternion orientation = Ogre::Quaternion(Ogre::Degree(-ratio * maxangle), (x_node_pos - ref_node_pos).normalisedCopy()) * Ogre::Quaternion(refx, normal, refy);
        abx.abx_scenenode->setOrientation(orientation);

    }
}

// TODO: Also move the data structure + setup code to GfxActor ~ only_a_ptr, 05/2018
void RoR::GfxActor::UpdateCParticles()
{
    //update custom particle systems
    NodeData* nodes = m_simbuf.simbuf_nodes.get();
    for (int i = 0; i < m_actor->ar_num_custom_particles; i++)
    {
        Ogre::Vector3 pos = nodes[m_actor->ar_custom_particles[i].emitterNode].AbsPosition;
        Ogre::Vector3 dir = pos - nodes[m_actor->ar_custom_particles[i].directionNode].AbsPosition;
        dir = fast_normalise(dir);
        m_actor->ar_custom_particles[i].snode->setPosition(pos);
        for (int j = 0; j < m_actor->ar_custom_particles[i].psys->getNumEmitters(); j++)
        {
            m_actor->ar_custom_particles[i].psys->getEmitter(j)->setDirection(dir);
        }
    }
}

void RoR::GfxActor::UpdateAeroEngines()
{
    for (int i = 0; i < m_actor->ar_num_aeroengines; i++)
    {
        m_actor->ar_aeroengines[i]->updateVisuals(this);
    }
}

void RoR::GfxActor::UpdateNetLabels(float dt)
{
    // TODO: Remake network player labels via GUI... they shouldn't be billboards inside the scene ~ only_a_ptr, 05/2018
    if (m_actor->m_net_label_node && m_actor->m_net_label_mt)
    {
        // this ensures that the nickname is always in a readable size
        Ogre::Vector3 label_pos = m_simbuf.simbuf_pos;
        label_pos.y += (m_simbuf.simbuf_aabb.getMaximum().y - m_simbuf.simbuf_aabb.getMinimum().y);
        m_actor->m_net_label_node->setPosition(label_pos);
        Ogre::Vector3 vdir = m_simbuf.simbuf_pos - gEnv->mainCamera->getPosition();
        float vlen = vdir.length();
        float h = std::max(0.6, vlen / 30.0);

        m_actor->m_net_label_mt->setCharacterHeight(h);
        if (vlen > 1000) // 1000 ... vlen
        {
            m_actor->m_net_label_mt->setCaption(
                m_simbuf.simbuf_net_username + "  (" + TOSTRING((float)(ceil(vlen / 100) / 10.0) ) + " km)");
        }
        else if (vlen > 20) // 20 ... vlen ... 1000
        {
            m_actor->m_net_label_mt->setCaption(
                m_simbuf.simbuf_net_username + "  (" + TOSTRING((int)vlen) + " m)");
        }
        else // 0 ... vlen ... 20
        {
            m_actor->m_net_label_mt->setCaption(m_simbuf.simbuf_net_username);
        }
    }
}
