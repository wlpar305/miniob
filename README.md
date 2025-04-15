# MiniOB-DBMS: 构建一个完整的数据库管理系统

本项目旨在基于 MiniOB 的基础，构建一个全面的数据库管理系统 (DBMS)。MiniOB 最初是 OceanBase 团队联合多所高校为数据库入门教学而设计的项目，而本分支旨在扩展其范围，致力于创建一个功能更完整、更健壮的数据库系统。

将利用 MiniOB 清晰的模块化设计，同时增强现有组件并添加生产级 DBMS 所需的新功能。

## 系统架构

系统采用模块化架构，通过一系列定义清晰的阶段处理 SQL 查询，并由阶段式事件驱动架构 (SEDA) 进行协调。

```mermaid
graph TD
    Client[客户端] --> Network[网络层];
    Network --> SessionStage[会话阶段];
    SessionStage --> ParseStage[解析阶段];
    ParseStage --> ResolveStage[语义分析阶段];
    ResolveStage --> OptimizeStage[优化阶段];
    OptimizeStage -- Physical Plan [物理计划] --> ExecuteStage[执行阶段];
    OptimizeStage -- Command [命令] --> ExecuteStage;
    ExecuteStage --> StorageEngine[存储引擎];
    StorageEngine --> ExecuteStage;
    ExecuteStage --> Network;


    subgraph SQL 处理流水线 (SEDA 阶段)
        direction LR
        SessionStage --> ParseStage --> ResolveStage --> OptimizeStage --> ExecuteStage
    end

    subgraph 核心组件
        Network[网络层 (`src/observer/net`)]
        StorageEngine[存储引擎 (`src/observer/storage`)]
        SessionManager[会话管理器 (`src/observer/session`)]
        EventManager[事件管理器 (`deps/common/seda`, `src/observer/event`)]
    end

    SessionStage -.-> SessionManager;
    ExecuteStage -.-> SessionManager;
    StorageEngine -.-> SessionManager;
    ExecuteStage -.-> EventManager;
    Network -.-> EventManager;

```

**核心模块与处理流程:**

1.  **网络层 (`src/observer/net`):**
    *   监听客户端连接 (例如，通过 `mysql_communicator.cpp` 实现 MySQL 协议)。
    *   接收 SQL 查询并发送回结果。
    *   通过 `Communicator` 管理连接状态。

2.  **会话管理器 (`src/observer/session`):**
    *   管理用户会话 (`Session`)，每个会话对应一个连接。
    *   跟踪会话状态，包括当前数据库 (`Db`) 和活动事务 (`Trx`)。

3.  **事件管理器 (`deps/common/seda`, `src/observer/event`):**
    *   实现阶段式事件驱动架构 (SEDA)，用于异步处理请求。
    *   定义事件，如 `SessionEvent` (初始请求) 和 `SQLStageEvent` (在 SQL 处理阶段间传递)。

4.  **SQL 处理流水线 (位于 `src/observer/session/session_stage.cpp` 中的 SEDA 阶段):**
    *   **`SessionStage`**: 入口点，接收 `SessionEvent`，创建 `SQLStageEvent`。
    *   **`ParseStage`**: 调用 SQL 解析器 (`src/observer/sql/parser`)。
        *   使用 Lex (`lex_sql.l`) 和 Yacc (`yacc_sql.y`) 进行词法和语法分析。
        *   生成初始的抽象语法树 (AST)，由 `ParsedSqlNode` 表示。
    *   **`ResolveStage`**: 执行语义分析。
        *   根据数据库模式和元数据 (表/列存在性、类型、权限 - *部分实现*) 验证 `ParsedSqlNode`。
        *   解析名称并将 AST 转换为内部表示 (`Stmt` 对象，定义在 `src/observer/sql/stmt`)。
    *   **`OptimizeStage`**: 生成高效的执行计划 (`src/observer/sql/optimizer`)。
        *   从 `Stmt` 创建逻辑计划 (`LogicalOperator`)。
        *   应用基于规则的重写 (`Rewriter`, 例如 `PredicatePushdownRewriter`)。
        *   (*未来: 基于代价的优化*)
        *   生成描述确切执行步骤的物理计划 (`PhysicalOperator`)。
        *   识别由 `CommandExecutor` 直接处理的 DDL/实用程序命令。
    *   **`ExecuteStage`**: 执行查询 (`src/observer/sql/executor`)。
        *   如果存在 `PhysicalOperator` 树，则迭代执行计划 (例如 `TableScanPhysicalOperator`, `IndexScanPhysicalOperator`, `PredicatePhysicalOperator`, `JoinPhysicalOperator`) 以产生结果。
        *   如果是命令 (如 `CREATE TABLE`)，则调用相应的 `CommandExecutor` (`src/observer/sql/executor/command_executor.h` 及具体执行器如 `create_table_executor.h`)。
        *   将结果 (数据行、受影响行数或错误) 填充到 `SqlResult` 对象中。

5.  **存储引擎 (`src/observer/storage`):**
    *   **缓冲池管理器 (`buffer/`):** 管理磁盘页 (`Page`) 在内存 (`Frame`) 中的缓存，以最小化磁盘 I/O。每个文件使用 `DiskBufferPool`，并共享 `BPFrameManager`。实现页面替换策略 (*基本实现*)。
    *   **记录管理器 (`record/`):** 处理页面内记录 (`Record`) 的存储布局。使用 `RecordFileHandler` (文件级) 和 `RecordPageHandler` (页面级)。记录由 `RID` (PageNum, SlotNum) 标识。
    *   **索引管理器 (`index/`):** 管理索引结构。包含 B+ 树实现 (`BPlusTreeIndex`)，用于基于键值的高效数据查找。提供 `IndexScanner` 用于索引遍历。
    *   **事务管理器 (`trx/`):** 管理事务 (`Trx`)，确保 ACID 属性 (*基本实现*)。协调日志记录和恢复。处理并发控制 (*最小实现*)。
    *   **日志管理器 (`clog/`):** 实现预写日志 (WAL)。`CLogManager` 将日志记录 (`CLogRecord`) 写入提交日志文件以用于恢复。处理事务开始、提交和回滚的日志条目。
    *   **元数据/数据库管理器 (`db/`, `table/`):** 管理数据库 (`Db`) 和表 (`Table`) 定义。`TableMeta` 存储模式信息 (字段、类型、索引)。处理表及其关联文件 (元数据、数据、索引) 的创建和打开。

## 快速上手

*(保留或更新构建/运行说明)*

1.  **环境依赖:** ...
2.  **编译:**
    ```bash
    # 确保已安装 cmake, make, gcc/g++, flex, bison
    bash build.sh --make -j4
    ```
3.  **运行服务端:**
    ```bash
    cd build/bin
    ./observer -f ../../etc/observer.ini
    ```
4.  **运行客户端:**
    ```bash
    cd build/bin
    ./client
    ```
    或者使用任何兼容 MySQL 协议的客户端连接到默认端口 `6789`。

# 任务要求

当前Codebase中包含了一个已经实现完毕的基于MiniOB内核的DBMS。你需要从整个Codebase中按要求找到并分析实现相应功能代码，并整理成一份Latex实验报告的一部分subsection。

## 代码检索要求

你需要从整个Codebase中找到有关完成以下任务的部分，并分析总结其实现思路：

在现有功能上实现日期类型字段。
当前已经支持了int、char、float类型，在此基础上实现date类型的字段。date测试可能超过2038年2月，也可能小于1970年1月1号。注意处理非法的date输入，需要返回FAILURE。
这道题目需要从词法解析开始，一直调整代码到执行阶段，还需要考虑DATE类型数据的存储。
注意：
- 需要考虑date字段作为索引时的处理，以及如何比较大小;
- 这里没有限制日期的范围，需要处理溢出的情况。
- 需要考虑闰年。

## 实验报告要求

请根据以上检索出的内容，整理并完成实验报告。要求如下：

- 整段实验报告应该在一个latex subsection内。内部只允许使用\paragraph，不可使用\subsubsection。
- 报告中写出各个处理步骤，并给出解释；每处都需要使用lstlisting环境贴上对应代码，并指定好[language=]
- 使用冷静的临床腔描述，大部分内容不加主语。
- 使用latex完成。只需要完成subsection内容即可。不需要写package相关，我会将其填充到模版中。
- 正确使用latex转义符，在需要时来转义特殊符号。
- 这是一份实验报告，需要格式按照实验报告的格式书写。非必要不能使用列举，不需要解释非常基础的概念比如curl是什么。

