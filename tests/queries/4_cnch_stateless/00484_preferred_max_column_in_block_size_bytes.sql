
set dialect_type='CLICKHOUSE';
set min_block_size=1;
drop table if exists tab;
create table tab (date Date, x UInt64, s FixedString(128)) engine = CnchMergeTree() PARTITION BY toYYYYMM(date) ORDER BY (date, x) SETTINGS index_granularity = 8192;
insert into tab select today(), number, toFixedString('', 128) from system.numbers limit 8192;

set preferred_block_size_bytes = 2000000;
set preferred_max_column_in_block_size_bytes = 0;
select max(blockSize()), min(blockSize()), any(ignore(*)) from tab;
set preferred_max_column_in_block_size_bytes = 128;
select max(blockSize()), min(blockSize()), any(ignore(*)) from tab;
set preferred_max_column_in_block_size_bytes = 256;
select max(blockSize()), min(blockSize()), any(ignore(*)) from tab;
set preferred_max_column_in_block_size_bytes = 2097152;
select max(blockSize()), min(blockSize()), any(ignore(*)) from tab;
set preferred_max_column_in_block_size_bytes = 4194304;
select max(blockSize()), min(blockSize()), any(ignore(*)) from tab;

drop table if exists tab;
create table tab (date Date, x UInt64, s FixedString(128)) engine = CnchMergeTree() PARTITION BY toYYYYMM(date) ORDER BY (date, x) SETTINGS index_granularity = 32;
insert into tab select today(), number, toFixedString('', 128) from system.numbers limit 47;
set preferred_max_column_in_block_size_bytes = 1152;
select blockSize(), * from tab where x = 1 or x > 36 format Null;

drop table if exists tab;
create table tab (date Date, x UInt64, s FixedString(128)) engine = CnchMergeTree() PARTITION BY toYYYYMM(date) ORDER BY (date, x) SETTINGS index_granularity = 8192;
insert into tab select today(), number, toFixedString('', 128) from system.numbers limit 10;
set preferred_max_column_in_block_size_bytes = 128;
select s from tab where s == '' format Null;

drop table if exists tab;
create table tab (date Date, x UInt64, s String) engine = CnchMergeTree() PARTITION BY toYYYYMM(date) ORDER BY (date, x) SETTINGS index_granularity = 8192;
insert into tab select today(), number, 'abc' from system.numbers limit 81920;
set preferred_block_size_bytes = 0;
select count(*) from tab prewhere s != 'abc' format Null;
select count(*) from tab prewhere s = 'abc' format Null;
