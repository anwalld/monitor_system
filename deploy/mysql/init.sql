-- linux-monitor 数据库表结构
-- MySQL 容器首次以空数据卷启动时由 /docker-entrypoint-initdb.d 自动执行一次。
-- 已执行过的环境不会重跑本文件；后续表结构变更以新增迁移脚本处理。
-- 数据库名由 compose 的 MYSQL_DATABASE 创建并选中，本文件不写死库名。

SET NAMES utf8mb4;

CREATE TABLE node (
  node_id    VARCHAR(64) PRIMARY KEY,
  addr       VARCHAR(64) NOT NULL,
  ncpu       INT NOT NULL,
  first_seen DATETIME NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 每秒样本在 Manager 内存里聚合，每 5 分钟写一行，写入次数降到约 1/300
CREATE TABLE node_metrics_5m (
  node_id          VARCHAR(64) NOT NULL,
  bucket_start     DATETIME NOT NULL,
  cpu_avg          FLOAT,
  cpu_max          FLOAT,
  runq_p99_max_us  FLOAT,
  mem_avail_min    FLOAT,
  net_rtt_avg_ms   FLOAT,
  net_timeout_rate FLOAT,
  net_retrans_rate FLOAT,
  score_avg        FLOAT,
  samples          INT,
  PRIMARY KEY (node_id, bucket_start)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE node_event (
  id      BIGINT AUTO_INCREMENT PRIMARY KEY,
  node_id VARCHAR(64) NOT NULL,
  ts      DATETIME(3) NOT NULL,
  kind    VARCHAR(32) NOT NULL,   -- evict / rescue_start / rescue_end / slow_start / normal / backoff
  detail  JSON,
  KEY idx_node_ts (node_id, ts)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE task (
  task_id     CHAR(26) PRIMARY KEY,   -- ULID
  node_id     VARCHAR(64) NOT NULL,
  input_bytes BIGINT NOT NULL,
  state       VARCHAR(16) NOT NULL,   -- allocated / sent / received / done / failed / timeout / void
  t_alloc     DATETIME(3) NOT NULL,
  t_sent      DATETIME(3),
  t_recv      DATETIME(3),
  t_done      DATETIME(3),
  KEY idx_node_alloc (node_id, t_alloc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE ai_diagnosis (
  id       BIGINT AUTO_INCREMENT PRIMARY KEY,
  node_id  VARCHAR(64) NOT NULL,
  ts       DATETIME(3) NOT NULL,
  severity VARCHAR(8) NOT NULL,
  anomaly  JSON NOT NULL,   -- 检测器给出的异常
  report   JSON NOT NULL,   -- LLM 结构化输出
  model    VARCHAR(64),
  tokens   INT,
  KEY idx_node_ts (node_id, ts)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE ai_daily_report (
  day        DATE PRIMARY KEY,
  stats      JSON NOT NULL,         -- 代码算好的数字
  markdown   MEDIUMTEXT NOT NULL,   -- LLM 写的正文
  created_at DATETIME NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
