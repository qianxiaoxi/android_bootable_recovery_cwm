/*
 * Copyright (c) 2013 The CyanogenMod Project
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

#include "voldclient.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "common.h"

int vold_update_volumes() {

    const char *cmd[2] = {"volume", "list"};
    return vold_command(2, cmd, 1);
}

int vold_mount_volume(const char* path, int wait) {

    const char *cmd[3] = { "volume", "mount", path };
    int state = vold_get_volume_state(path);

    if (state == State_Mounted) {
        LOGI("卷 %s 已挂载\n", path);
        return 0;
    }

    if (state != State_Idle) {
        LOGI("卷 %s 并未闲置，当前状态为：%d\n", path, state);
        return -1;
    }

    if (access(path, R_OK) != 0) {
        mkdir(path, 0000);
        chown(path, 1000, 1000);
    }
    return vold_command(3, cmd, wait);
}

int vold_unmount_volume(const char* path, int force, int wait) {

    const char *cmd[4] = { "volume", "unmount", path, "force" };
    int state = vold_get_volume_state(path);

    if (state <= State_Idle) {
        LOGI("卷 %s 未挂载\n", path);
        return 0;
    }

    if (state != State_Mounted) {
        LOGI("卷 %s 无法在状态为 %d 时卸载\n", path, state);
        return -1;
    }

    return vold_command(force ? 4: 3, cmd, wait);
}

int vold_share_volume(const char* path) {

    const char *cmd[4] = { "volume", "share", path, "ums" };
    int state = vold_get_volume_state(path);

    if (state == State_Mounted)
        vold_unmount_volume(path, 0, 1);

    return vold_command(4, cmd, 1);
}

int vold_unshare_volume(const char* path, int mount) {

    const char *cmd[4] = { "volume", "unshare", path, "ums" };
    int state = vold_get_volume_state(path);
    int ret = 0;

    if (state != State_Shared) {
        LOGE("卷 %s 未共享 - 状态=%d\n", path, state);
        return 0;
    }

    ret = vold_command(4, cmd, 1);

    if (mount)
        vold_mount_volume(path, 1);

    return ret;
}

int vold_format_volume(const char* path, int wait) {

    const char* cmd[3] = { "volume", "format", path };
    return vold_command(3, cmd, wait);
}

int vold_custom_format_volume(const char* path, const char* fstype, int wait) {
    const char* cmd[4] = { "volume", "format", path, fstype };
    return vold_command(4, cmd, wait);
}

const char* volume_state_to_string(int state) {
    if (state == State_Init)
        return "初始化中";
    else if (state == State_NoMedia)
        return "非大容量存储设备";
    else if (state == State_Idle)
        return "空闲-未挂载";
    else if (state == State_Pending)
        return "挂起中";
    else if (state == State_Mounted)
        return "已挂载";
    else if (state == State_Unmounting)
        return "卸载中";
    else if (state == State_Checking)
        return "检查中";
    else if (state == State_Formatting)
        return "格式化中";
    else if (state == State_Shared)
        return "共享-未挂载";
    else if (state == State_SharedMnt)
        return "共享-已挂载";
    else
        return "未知错误";
}

