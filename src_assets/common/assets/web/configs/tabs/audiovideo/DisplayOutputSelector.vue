<script setup>
import { ref, computed } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../PlatformLayout.vue'

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
const isPipeWire = computed(() => config.value.capture === 'pipewire')
const outputNamePlaceholder = computed(() => {
  if (isPipeWire.value) return 'pw:v4l2_output.pci-0000_03_00.0-card'
  return (props.platform === 'windows') ? '{de9bb7e2-186e-505b-9e93-f48793333810}' : '0'
})
</script>

<template>
  <div class="mb-3">
    <label for="output_name" class="form-label">{{ $t('config.output_name') }}</label>
    <input type="text" class="form-control" id="output_name" :placeholder="outputNamePlaceholder"
           v-model="config.output_name"/>
    <div class="form-text">
      {{ $tp('config.output_name_desc') }}<br>
      <PlatformLayout :platform="platform">
        <template #windows>
          <pre style="white-space: pre-line;">
            <b>&nbsp;&nbsp;{</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"device_id": "{de9bb7e2-186e-505b-9e93-f48793333810}"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"display_name": "\\\\.\\DISPLAY1"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"friendly_name": "ROG PG279Q"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;...</b>
            <b>&nbsp;&nbsp;}</b>
          </pre>
        </template>
        <template #freebsd>
          <pre style="white-space: pre-line;">
            Info: Detecting displays
            Info: Detected display: DVI-D-0 (id: 0) connected: false
            Info: Detected display: HDMI-0 (id: 1) connected: true
            Info: Detected display: DP-0 (id: 2) connected: true
            Info: Detected display: DP-1 (id: 3) connected: false
            Info: Detected display: DVI-D-1 (id: 4) connected: false
          </pre>
        </template>
        <template #linux>
          <div v-if="isPipeWire">
            <pre style="white-space: pre-line;">
            PipeWire node: id=42 name='v4l2_output.pci-0000_03_00.0-card' desc='Screen capture' class='Video/Source'
            PipeWire node: id=58 name='gamescope' desc='Gamescope output' class='Stream/Output/Video'
            </pre>
            <small>Use <code>pw:&lt;node_name&gt;</code> or <code>pw:&lt;node_id&gt;</code>. Leave empty to use the first available video source. Run <code>pw-cli list-objects</code> to find node names.</small>
          </div>
          <div v-else>
            <pre style="white-space: pre-line;">
            Info: Detecting displays
            Info: Detected display: DVI-D-0 (id: 0) connected: false
            Info: Detected display: HDMI-0 (id: 1) connected: true
            Info: Detected display: DP-0 (id: 2) connected: true
            Info: Detected display: DP-1 (id: 3) connected: false
            Info: Detected display: DVI-D-1 (id: 4) connected: false
            </pre>
          </div>
        </template>
        <template #macos>
          <pre style="white-space: pre-line;">
            Info: Detecting displays
            Info: Detected display: Monitor-0 (id: 3) connected: true
            Info: Detected display: Monitor-1 (id: 2) connected: true
          </pre>
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
