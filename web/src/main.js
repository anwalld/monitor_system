import { createApp } from "vue";
import { createRouter, createWebHashHistory } from "vue-router";
import App from "./App.vue";
import Overview from "./pages/Overview.vue";
import NodeDetail from "./pages/NodeDetail.vue";
import Events from "./pages/Events.vue";
import Diagnoses from "./pages/Diagnoses.vue";
import Settings from "./pages/Settings.vue";
import Chat from "./pages/Chat.vue";
import Reports from "./pages/Reports.vue";
import { connect } from "./live.js";
import "./style.css";

const router = createRouter({
  history: createWebHashHistory(),
  routes: [
    { path: "/", component: Overview },
    { path: "/nodes/:id", component: NodeDetail, props: true },
    { path: "/events", component: Events },
    { path: "/diagnoses", component: Diagnoses },
    { path: "/chat", component: Chat },
    { path: "/reports", component: Reports },
    { path: "/settings", component: Settings },
  ],
});

connect();
createApp(App).use(router).mount("#app");
