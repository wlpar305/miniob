/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2023/6/13.
//

#include "sql/stmt/create_view_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "event/sql_debug.h"
#include "storage/db/db.h"

RC CreateViewStmt::create(Db *db, const CreateViewSqlNode &create_view, Stmt *&stmt)
{
  RC rc = RC::SUCCESS;

  LOG_INFO("Creating view: %s", create_view.view_name.c_str());

  // 检查视图名是否已存在（检查表名和视图名）
  if (db->find_table(create_view.view_name.c_str()) != nullptr) {
    LOG_WARN("Table with name %s already exists", create_view.view_name.c_str());
    return RC::SCHEMA_TABLE_EXIST;
  }
  
  if (db->find_view(create_view.view_name.c_str()) != nullptr) {
    LOG_WARN("View %s already exists", create_view.view_name.c_str());
    return RC::SCHEMA_TABLE_EXIST;
  }

  // 创建SELECT语句
  SelectStmt *select_stmt = nullptr;
  if (create_view.select_sql_node != nullptr) {
    LOG_INFO("Creating SELECT statement for view %s", create_view.view_name.c_str());
    rc = SelectStmt::create(db, create_view.select_sql_node->selection, reinterpret_cast<Stmt *&>(select_stmt));
    if (rc != RC::SUCCESS) {
      LOG_WARN("Failed to create select statement for view %s, rc=%s", create_view.view_name.c_str(), strrc(rc));
      return rc;
    }
    LOG_INFO("Successfully created SELECT statement for view %s", create_view.view_name.c_str());
  } else {
    LOG_WARN("No select statement provided for view %s", create_view.view_name.c_str());
    return RC::INVALID_ARGUMENT;
  }

  stmt = new CreateViewStmt(db, create_view.view_name, select_stmt);
  LOG_INFO("Successfully created CreateViewStmt for view %s", create_view.view_name.c_str());
  return rc;
} 