<script setup>
import { ref, computed } from "vue";
import { live, setToken, reconnectNow } from "./live.js";

const tokenInput = ref(live.token);
const statusText = computed(() => ({
  open: "已连接", connecting: "连接中…", closed: `已断开，${live.retryIn} 秒后重连`, unauthorized: "token 无效",
}[live.status]));
const statusColor = computed(() => ({
  open: "var(--ok)", connecting: "var(--warn)", closed: "var(--bad)", unauthorized: "var(--bad)",
}[live.status]));
</script>

<template>
  <header class="top">
    <h1>节点监控看板</h1>
    <nav>
      <router-link to="/">总览</router-link>
      <router-link to="/events">事件</router-link>
    </nav>
    <span class="spacer"></span>
    <span class="conn" :title="`连接次数 ${live.connects}，最新事件 ${live.lastEventId || '-'}`">
      <i class="dot" :style="{ background: statusColor }"></i>{{ statusText }}
      <button v-if="live.status === 'closed'" @click="reconnectNow">立即重连</button>
    </span>
    <form v-if="live.status === 'unauthorized'" class="conn" @submit.prevent="setToken(tokenInput)">
      <input v-model="tokenInput" placeholder="访问 token" size="14" type="password" />
      <button type="submit">保存</button>
    </form>
  </header>
  <main><router-view /></main>
</template>
