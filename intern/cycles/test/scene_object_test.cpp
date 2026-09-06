/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <gtest/gtest.h>

#include "scene/light.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/volume.h"
#include "session/session.h"

CCL_NAMESPACE_BEGIN

TEST(ObjectManager, BackgroundObjectIndex)
{
  SessionParams params;
  params.device = Device::available_devices(DEVICE_MASK_CPU).front();
  params.denoise_device = params.device;
  params.background = true;
  params.headless = true;
  params.threads = 1;
  Session session(params, SceneParams());
  Scene *scene = session.scene.get();

  const auto update = [&]() {
    scene->object_manager->tag_update(scene, ObjectManager::UPDATE_ALL);
    scene->object_manager->device_update(
        session.device.get(), &scene->dscene, scene, session.progress);
  };

  update();
  EXPECT_EQ(scene->dscene.data.background.object_index, OBJECT_NONE);

  Volume *volume = scene->create_node<Volume>();
  Object *object = scene->create_node<Object>();
  object->set_geometry(volume);
  update();
  EXPECT_EQ(object->index, 0);
  EXPECT_EQ(scene->dscene.data.background.object_index, OBJECT_NONE);

  BackgroundLight *light = scene->create_node<BackgroundLight>();
  Object *world = scene->create_node<Object>();
  world->set_geometry(light);
  update();
  EXPECT_EQ(scene->dscene.data.background.object_index, world->index);

  /* Removing another object changes the world object's index. */
  scene->delete_node(object);
  update();
  EXPECT_EQ(world->index, 0);
  EXPECT_EQ(scene->dscene.data.background.object_index, world->index);

  object = scene->create_node<Object>();
  object->set_geometry(volume);
  scene->delete_node(world);
  update();
  EXPECT_EQ(object->index, 0);
  EXPECT_EQ(scene->dscene.data.background.object_index, OBJECT_NONE);

  scene->delete_node(object);
  update();
  EXPECT_EQ(scene->dscene.data.background.object_index, OBJECT_NONE);
}

CCL_NAMESPACE_END
