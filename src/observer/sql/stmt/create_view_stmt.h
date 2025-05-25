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

#include "sql/stmt/stmt.h"

class Db;
class SelectStmt;

/**
 * @brief 表示创建视图的语句
 * @ingroup Statement
 */
class CreateViewStmt : public Stmt
{
public:
  CreateViewStmt(Db *db, const std::string &view_name, SelectStmt *select_stmt)
        : db_(db),
          view_name_(view_name),
          select_stmt_(select_stmt)
  {}
  virtual ~CreateViewStmt()
  {
    if (nullptr != select_stmt_) {
      delete select_stmt_;
    }
  }

  StmtType type() const override { return StmtType::CREATE_VIEW; }

  const std::string &view_name() const { return view_name_; }
  SelectStmt *select_stmt() const { return select_stmt_; }

  static RC create(Db *db, const CreateViewSqlNode &create_view, Stmt *&stmt);

  Db *get_db() const { return db_; }

private:
  Db *db_ = nullptr;
  std::string view_name_;
  SelectStmt *select_stmt_ = nullptr;
}; 