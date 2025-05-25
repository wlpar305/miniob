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

#include "storage/table/view.h"
#include "sql/stmt/select_stmt.h"
#include "common/log/log.h"

RC View::init(const char *view_name, SelectStmt *select_stmt)
{
  if (view_name == nullptr || select_stmt == nullptr) {
    LOG_WARN("Invalid arguments for view initialization");
    return RC::INVALID_ARGUMENT;
  }

  name_ = view_name;
  select_stmt_ = select_stmt;

  LOG_INFO("View %s initialized successfully", view_name);
  return RC::SUCCESS;
} 