--
-- Filter views
--
CREATE VIEW IF NOT EXISTS
    "rocpd_node" AS
SELECT
    *
FROM
    _rocpd_node;

CREATE VIEW IF NOT EXISTS
    "rocpd_process" AS
SELECT
    *
FROM
    _rocpd_process;

CREATE VIEW IF NOT EXISTS
    "rocpd_thread" AS
SELECT
    *
FROM
    _rocpd_thread;

CREATE VIEW IF NOT EXISTS
    "rocpd_track" AS
SELECT
    *
FROM
    _rocpd_track T;

CREATE VIEW IF NOT EXISTS
    "rocpd_region" AS
SELECT
    *
FROM
    _rocpd_region;

CREATE VIEW IF NOT EXISTS
    "rocpd_sample" AS
SELECT
    *
FROM
    _rocpd_sample;

CREATE VIEW IF NOT EXISTS
    "rocpd_kernel_dispatch" AS
SELECT
    *
FROM
    _rocpd_kernel_dispatch;

CREATE VIEW IF NOT EXISTS
    "rocpd_memory_copy" AS
SELECT
    *
FROM
    _rocpd_memory_copy;

CREATE VIEW IF NOT EXISTS
    "rocpd_memory_allocate" AS
SELECT
    *
FROM
    _rocpd_memory_allocate;

--
-- Useful views
--
--
-- CPU regions
CREATE VIEW IF NOT EXISTS
    regions AS
SELECT
    R.id,
    (
        SELECT
            string
        FROM
            rocpd_string
        WHERE
            rocpd_string.id = E.category_id
    ) AS category,
    S.string AS name,
    R.node_id AS node,
    R.pid,
    R.tid,
    R.start,
    R.end,
    (R.end - R.start) AS duration,
    E.stack_id AS stack_id,
    E.parent_stack_id AS parent_stack_id,
    E.correlation_id AS corr_id,
    E.args AS args,
    E.extdata AS extdata,
    E.metrics AS metrics,
    E.call_stack AS call_stack,
    E.line_info AS line_info
FROM
    rocpd_region R
    INNER JOIN rocpd_event E ON E.id = R.event_id
    INNER JOIN rocpd_string S ON S.id = R.name_id;

--
-- Samples
CREATE VIEW IF NOT EXISTS
    samples AS
SELECT
    R.id,
    (
        SELECT
            string
        FROM
            rocpd_string
        WHERE
            rocpd_string.id = E.category_id
    ) AS category,
    (
        SELECT
            string
        FROM
            rocpd_string
        WHERE
            rocpd_string.id = T.name_id
    ) AS name,
    T.node_id AS node,
    T.pid,
    T.tid,
    R.timestamp,
    E.stack_id AS stack_id,
    E.parent_stack_id AS parent_stack_id,
    E.correlation_id AS corr_id,
    E.args AS args,
    E.extdata AS extdata,
    E.metrics AS metrics,
    E.call_stack AS call_stack,
    E.line_info AS line_info
FROM
    rocpd_sample R
    INNER JOIN rocpd_track T ON T.id = R.track_id
    INNER JOIN rocpd_event E ON E.id = R.event_id;

--
-- Kernel information
CREATE VIEW
    kernels AS
SELECT
    K.id,
    (
        SELECT
            string
        FROM
            rocpd_string
        WHERE
            rocpd_string.id = E.category_id
    ) AS category,
    R.string AS region,
    S.display_name AS name,
    K.node_id,
    A.absolute_index AS agent_index,
    A.type_index AS gpu_index,
    S.code_object_id AS code_object_id,
    K.kernel_id,
    K.dispatch_id,
    Q.name AS queue,
    ST.name AS stream,
    K.start,
    K.end,
    (K.end - K.start) AS duration,
    K.grid_size_x AS grid_x,
    K.grid_size_y AS grid_y,
    K.grid_size_z AS grid_z,
    K.workgroup_size_x AS workgroup_x,
    K.workgroup_size_y AS workgroup_y,
    K.workgroup_size_z AS workgroup_z,
    K.group_segment_size AS lds_size,
    K.private_segment_size AS scratch_size,
    S.group_segment_size AS static_lds_size,
    S.private_segment_size AS static_scratch_size,
    E.correlation_id AS corr_id,
    E.metrics AS metrics
FROM
    rocpd_kernel_dispatch K
    INNER JOIN rocpd_agent A ON A.id = K.agent_id
    INNER JOIN rocpd_event E ON E.id = K.event_id
    INNER JOIN rocpd_string R ON R.id = K.region_name_id
    INNER JOIN rocpd_kernel_symbol S ON S.id = K.kernel_id
    LEFT JOIN rocpd_stream ST ON ST.id = K.stream_id
    LEFT JOIN rocpd_queue Q ON Q.id = K.queue_id;

--
-- Performance Monitoring Counters (PMC)
CREATE VIEW IF NOT EXISTS
    pmc_events AS
SELECT
    PMC_E.id,
    E.id AS event_id,
    (
        SELECT
            string
        FROM
            rocpd_string
        WHERE
            rocpd_string.id = E.category_id
    ) AS category,
    (
        SELECT
            display_name
        FROM
            rocpd_kernel_symbol
        WHERE
            rocpd_kernel_symbol.id = K.kernel_id
    ) AS name,
    K.node_id AS node,
    K.dispatch_id,
    K.start,
    K.end,
    (K.end - K.start) AS duration,
    PMC_I.name AS counter_name,
    PMC_E.value AS counter_value
FROM
    rocpd_pmc_event PMC_E
    INNER JOIN rocpd_pmc PMC_I ON PMC_I.id = PMC_E.pmc_id
    INNER JOIN rocpd_event E ON E.id = PMC_E.event_id
    INNER JOIN rocpd_kernel_dispatch K ON K.event_id = PMC_E.event_id;

-- events with arguments ---
CREATE VIEW IF NOT EXISTS
    events_args AS
SELECT
    E.id AS event_id,
    (
        SELECT
            string
        FROM
            rocpd_string
        WHERE
            rocpd_string.id = E.category_id
    ) AS category,
    E.correlation_id,
    E.stack_id,
    E.parent_stack_id,
    A.position AS arg_position,
    A.type AS arg_type,
    A.name AS arg_name,
    A.value AS arg_value,
    E.metrics,
    E.call_stack,
    E.line_info,
    A.extdata
FROM
    rocpd_event E
    INNER JOIN rocpd_arg A ON A.event_id = E.id;

-- list of astream arguments enriched by the corresponding stream descriptions
CREATE VIEW IF NOT EXISTS
    stream_args AS
SELECT 
    A.id AS argument_id,
    A.event_id AS event_id,
    A.position AS arg_position,
    A.type AS arg_type,
    A.value AS arg_value,
    json_extract(A.extdata, '$.stream_id') AS stream_id,
    S.node_id AS node_id,
    S.pid AS pid,
    S.name AS stream_name,
    S.extdata as extdata
FROM
    rocpd_arg A
    INNER JOIN rocpd_stream S ON json_extract(A.extdata, '$.stream_id') = S.id
WHERE  
    A.name = 'stream';


--
-- Sorted list of kernels which consume the most overall time
CREATE VIEW IF NOT EXISTS
    top_kernels AS
SELECT
    S.display_name AS name,
    COUNT(K.kernel_id) AS total_calls,
    SUM(K.end - K.start) / 1000 AS total_duration,
    (SUM(K.end - K.start) / COUNT(K.kernel_id)) / 1000 AS average,
    SUM(K.end - K.start) * 100.0 / (
        SELECT
            SUM(A.end - A.start)
        FROM
            rocpd_kernel_dispatch A
    ) AS percentage
FROM
    rocpd_kernel_dispatch K
    INNER JOIN rocpd_kernel_symbol S ON S.id = K.kernel_id
GROUP BY
    name
ORDER BY
    total_duration DESC;

--
-- GPU utilization metrics including kernels and memory copy operations
CREATE VIEW IF NOT EXISTS
    busy AS
SELECT
    A.agent_id,
    AG.type,
    GpuTime,
    WallTime,
    GpuTime * 1.0 / WallTime AS Busy
FROM
    (
        SELECT
            agent_id,
            SUM(end - start) AS GpuTime
        FROM
            (
                SELECT
                    agent_id,
                    end,
                    start
                FROM
                    rocpd_kernel_dispatch
                UNION ALL
                SELECT
                    dst_agent_id AS agent_id,
                    end,
                    start
                FROM
                    rocpd_memory_copy
            )
        GROUP BY
            agent_id
    ) A
    INNER JOIN (
        SELECT
            MAX(end) - MIN(start) AS WallTime
        FROM
            (
                SELECT
                    end,
                    start
                FROM
                    rocpd_kernel_dispatch
                UNION ALL
                SELECT
                    end,
                    start
                FROM
                    rocpd_memory_copy
            )
    ) W ON 1 = 1
    INNER JOIN rocpd_agent AG ON AG.id = A.agent_id;

--
-- Overall performance summary including kernels and memory copy operations
CREATE VIEW
    top AS
SELECT
    name,
    COUNT(*) AS total_calls,
    SUM(duration) / 1000.0 AS total_duration,
    (SUM(duration) / COUNT(*)) / 1000.0 AS average,
    SUM(duration) * 100.0 / total_time AS percentage
FROM
    (
        -- Kernel operations
        SELECT
            ks.kernel_name AS name,
            (kd.end - kd.start) AS duration
        FROM
            rocpd_kernel_dispatch kd
            INNER JOIN rocpd_kernel_symbol ks ON kd.kernel_id = ks.id
        UNION ALL
        -- Memory operations
        SELECT
            rs.string AS name,
            (end - start) AS duration
        FROM
            rocpd_memory_copy mc
            INNER JOIN rocpd_string rs ON rs.id = mc.name_id
        UNION ALL
        -- Regions
        SELECT
            rs.string AS name,
            (end - start) AS duration
        FROM
            rocpd_region rr
            INNER JOIN rocpd_string rs ON rs.id = rr.name_id
    ) operations
    CROSS JOIN (
        SELECT
            SUM(end - start) AS total_time
        FROM
            (
                SELECT
                    end,
                    start
                FROM
                    rocpd_kernel_dispatch
                UNION ALL
                SELECT
                    end,
                    start
                FROM
                    rocpd_memory_copy
                UNION ALL
                SELECT
                    end,
                    start
                FROM
                    rocpd_region
            )
    ) TOTAL
GROUP BY
    name
ORDER BY
    total_duration DESC;

--
--
CREATE VIEW IF NOT EXISTS
    memory_copies AS
SELECT
    M.id,
    (
        SELECT
            string
        FROM
            rocpd_string
        WHERE
            rocpd_string.id = E.category_id
    ) AS category,
    M.pid,
    M.tid,
    M.start,
    M.end,
    (M.end - M.start) AS duration,
    S.string AS name,
    R.string AS region_name,
    M.stream_id,
    M.queue_id,
    ST.name AS stream_name,
    Q.name AS queue_name,
    M.size,
    dst_agent.name AS dst_device,
    M.dst_address,
    src_agent.name AS src_device,
    M.src_address,
    E.correlation_id AS corr_id,
    E.metrics,
    E.args
FROM
    rocpd_memory_copy M
    INNER JOIN rocpd_string S ON S.id = M.name_id
    LEFT JOIN rocpd_string R ON R.id = M.region_name_id
    INNER JOIN rocpd_agent dst_agent ON dst_agent.id = M.dst_agent_id
    INNER JOIN rocpd_agent src_agent ON src_agent.id = M.src_agent_id
    LEFT JOIN rocpd_queue Q ON Q.id = M.queue_id
    LEFT JOIN rocpd_stream ST ON ST.id = M.stream_id
    INNER JOIN rocpd_event E ON E.id = M.event_id;

--
--
CREATE VIEW IF NOT EXISTS
    memory_allocation AS
SELECT
    M.id,
    (
        SELECT
            string
        FROM
            rocpd_string
        WHERE
            rocpd_string.id = E.category_id
    ) AS category,
    M.pid,
    M.tid,
    M.start,
    M.end,
    (M.end - M.start) AS duration,
    M.type,
    M.level,
    A.name AS agent_name,
    M.address,
    M.size,
    M.queue_id,
    Q.name AS queue_name,
    M.stream_id,
    ST.name AS stream_name,
    E.correlation_id AS corr_id,
    E.metrics,
    E.args
FROM
    rocpd_memory_allocate M
    INNER JOIN rocpd_agent A ON A.id = M.agent_id
    LEFT JOIN rocpd_queue Q ON Q.id = M.queue_id
    LEFT JOIN rocpd_stream ST ON ST.id = M.stream_id
    INNER JOIN rocpd_event E ON E.id = M.event_id;

--
--
CREATE VIEW
    range_markers AS
SELECT
    E.id AS event_id,
    JSON_EXTRACT(E.extdata, '$.message') AS name,
    ST.string AS marker_type,
    R.start,
    R.end,
    (R.end - R.start) AS duration,
    E.correlation_id,
    E.stack_id,
    E.parent_stack_id,
    E.args,
    E.metrics,
    E.call_stack,
    E.line_info
FROM
    rocpd_event E
    INNER JOIN rocpd_string ST ON ST.id = E.category_id
    INNER JOIN rocpd_region R ON R.event_id = E.id
WHERE
    ST.string LIKE '%MARKER%'
    AND JSON_VALID(E.extdata);

--
--
CREATE VIEW
    single_markers AS
SELECT
    E.id AS event_id,
    JSON_EXTRACT(E.extdata, '$.message') AS name,
    ST.string AS marker_type,
    SP.timestamp AS timestamp,
    E.correlation_id,
    E.stack_id,
    E.parent_stack_id,
    E.args,
    E.metrics,
    E.call_stack,
    E.line_info
FROM
    rocpd_event E
    INNER JOIN rocpd_string ST ON ST.id = E.category_id
    INNER JOIN rocpd_sample SP ON SP.event_id = E.id
WHERE
    ST.string LIKE '%MARKER%'
    AND JSON_VALID(E.extdata);

--
--
CREATE VIEW
    markers AS
SELECT
    *
FROM
    range_markers
UNION
SELECT
    SM.event_id,
    SM.name,
    SM.marker_type,
    SM.timestamp AS start,
    SM.timestamp AS end,
    0 AS duration,
    SM.correlation_id,
    SM.stack_id,
    SM.parent_stack_id,
    SM.args,
    SM.metrics,
    SM.call_stack,
    SM.line_info
FROM
    single_markers SM;

--
-- summaries
--
CREATE VIEW
    kernel_summary AS
WITH
    avg_data AS (
        SELECT
            name,
            AVG(duration) AS avg_duration
        FROM
            kernels
        GROUP BY
            name
    ),
    aggregated_data AS (
        SELECT
            K.name,
            COUNT(*) AS calls,
            SUM(K.duration) AS total_duration,
            A.avg_duration AS average_duration,
            MIN(K.duration) AS min_duration,
            MAX(K.duration) AS max_duration,
            SQRT(AVG((K.duration - A.avg_duration) * (K.duration - A.avg_duration))) AS std_dev_duration
        FROM
            kernels K
            JOIN avg_data A ON K.name = A.name
        GROUP BY
            K.name
    ),
    total_duration AS (
        SELECT
            SUM(total_duration) AS grand_total_duration
        FROM
            aggregated_data
    )
SELECT
    AD.name AS name,
    AD.calls,
    AD.total_duration AS "DURATION (nsec)",
    AD.average_duration AS "AVERAGE (nsec)",
    (CAST(AD.total_duration AS REAL) / TD.grand_total_duration) * 100 AS "PERCENT (INC)",
    AD.min_duration AS "MIN (nsec)",
    AD.max_duration AS "MAX (nsec)",
    AD.std_dev_duration AS "STD_DEV"
FROM
    aggregated_data AD
    CROSS JOIN total_duration TD;

--
--
CREATE VIEW
    range_marker_summary AS
WITH
    avg_data AS (
        SELECT
            name,
            AVG(duration) AS avg_duration
        FROM
            range_markers
        GROUP BY
            name
    ),
    aggregated_data AS (
        SELECT
            RM.name,
            COUNT(*) AS calls,
            SUM(RM.duration) AS total_duration,
            A.avg_duration AS average_duration,
            MIN(RM.duration) AS min_duration,
            MAX(RM.duration) AS max_duration,
            SQRT(AVG((RM.duration - A.avg_duration) * (RM.duration - A.avg_duration))) AS std_dev_duration
        FROM
            range_markers RM
            JOIN avg_data A ON RM.name = A.name
        GROUP BY
            RM.name
    ),
    total_duration AS (
        SELECT
            SUM(total_duration) AS grand_total_duration
        FROM
            aggregated_data
    )
SELECT
    AD.name AS name,
    AD.calls,
    AD.total_duration AS "DURATION (nsec)",
    AD.average_duration AS "AVERAGE (nsec)",
    (CAST(AD.total_duration AS REAL) / TD.grand_total_duration) * 100 AS "PERCENT (INC)",
    AD.min_duration AS "MIN (nsec)",
    AD.max_duration AS "MAX (nsec)",
    AD.std_dev_duration AS "STD_DEV"
FROM
    aggregated_data AD
    CROSS JOIN total_duration TD;

--
--
CREATE VIEW
    single_marker_summary AS
SELECT
    SM.name,
    COUNT(*) AS calls
FROM
    single_markers SM
GROUP BY
    SM.name;


--
-- Simplified Samples View (PMC)
CREATE VIEW IF NOT EXISTS
    samples_minimal_view AS
SELECT
    PMC_E.id,
    S.timestamp,
    PMC_I.name AS counter_name,
    PMC_E.value AS counter_value
FROM
    rocpd_pmc_event PMC_E
    INNER JOIN rocpd_pmc PMC_I ON PMC_I.id = PMC_E.pmc_id
    INNER JOIN rocpd_event E ON E.id = PMC_E.event_id
    INNER JOIN rocpd_sample S ON S.event_id = E.id;


--
-- RocmSMI (PMC)
CREATE VIEW IF NOT EXISTS
    rocm_smi AS
SELECT
    PMC_E.id,
    S.timestamp,
    A.type_index AS gpu_index,
    PMC_I.name AS counter_name,
    PMC_E.value AS counter_value,  
    PMC_I.description AS counter_description
FROM
    rocpd_pmc_event PMC_E
    INNER JOIN rocpd_pmc PMC_I ON PMC_I.id = PMC_E.pmc_id
    INNER JOIN rocpd_agent A ON A.id = PMC_I.agent_id
    INNER JOIN rocpd_event E ON E.id = PMC_E.event_id
    INNER JOIN rocpd_sample S ON S.event_id = E.id;

