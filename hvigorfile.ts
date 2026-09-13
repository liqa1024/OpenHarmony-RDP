// @ts-nocheck
//
// HmRdp - 工程级 Hvigor 配置。
//
// 签名配置**不放进仓库**：`build-profile.json5` 的 `signingConfigs` 恒为 `[]`，
// 本机的签名信息放在 `.signing/signing-config.json`（已 gitignore），由这里动态注入。
// 于是仓库里永远不会出现签名路径/口令，别人 clone 下来只是构建出未签名 HAP。
//
// 为什么走这条路：DevEco「自动签名」写进 build-profile.json5 的 storePassword/keyPassword
// 是**它自己加密的密文**，只有 DevEco/hvigor 能解（外部工具如 hap-sign-tool 用不了，那边要明文）。
// 所以正确做法是让 hvigor 照常签名，只是把那段配置搬到仓库外的文件里。
//
// 若 DevEco 重新自动签名（会把 signingConfigs 又写回 build-profile.json5）：
//   1) 把它写的那段 material 覆盖到 `.signing/signing-config.json`；
//   2) 把 build-profile.json5 的 signingConfigs 改回 `[]`（见 AGENTS.md）。
//
// 官方依据：FAQ「项目多人开发时会导致signingConfigs冲突」方式二（签名信息外置化 + overrides），
// 以及《动态修改编译配置》中的 `config.ohos.overrides.signingConfig`。
import { appTasks } from '@ohos/hvigor-ohos-plugin';
import * as fs from 'fs';
import * as path from 'path';

function loadLocalSigningConfig() {
  const file = path.join(__dirname, '.signing', 'signing-config.json');
  if (!fs.existsSync(file)) {
    return null;
  }
  try {
    return JSON.parse(fs.readFileSync(file, 'utf-8'));
  } catch (e) {
    // 文件损坏时按"没有签名配置"处理：构建仍可用，只是产出未签名 HAP。
    return null;
  }
}

const localSigningConfig = loadLocalSigningConfig();

export default {
  system: appTasks /* Built-in plugin of Hvigor. It cannot be modified. */,
  plugins: [] /* Custom plugin to extend the functionality of Hvigor. */,
  config: {
    ohos: {
      overrides: localSigningConfig ? { signingConfig: localSigningConfig } : {},
    },
  },
};
