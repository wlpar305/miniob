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

#pragma once

#include <string>
#include <memory>

#include "common/rc.h"

class SelectStmt;
class Table;

/**
 * @brief 视图类
 * @details 视图是一个虚拟表，它是基于一个或多个表的查询结果
 */
class View
{
public:
  View() = default;
  ~View() = default;

  /**
   * @brief 初始化视图
   * @param view_name 视图名称
   * @param select_stmt 视图对应的SELECT语句
   */
  RC init(const char *view_name, SelectStmt *select_stmt);

  /**
   * @brief 获取视图名称
   */
  const char *name() const { return name_.c_str(); }

  /**
   * @brief 获取视图对应的SELECT语句
   */
  SelectStmt *select_stmt() const { return select_stmt_; }

  /**
   * @brief 检查视图是否有效
   */
  bool is_valid() const { return !name_.empty() && select_stmt_ != nullptr; }

private:
  std::string name_;
  SelectStmt *select_stmt_ = nullptr;
}; 