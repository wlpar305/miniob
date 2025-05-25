-- 测试CREATE VIEW功能
-- 1. 测试引用不存在的表
create view test_view1 as select * from non_existent_table;

-- 2. 先创建一个表，然后基于它创建视图
create table test_table(id int, name char);
create view test_view2 as select * from test_table;

-- 3. 测试重复创建视图
create view test_view2 as select * from test_table; 