\subsection{字段与表别名功能实现}

字段和表别名功能增强了SQL查询的表达能力和结果的可读性。其实现涉及解析、语义分析、内部表示和执行等多个阶段。

\paragraph{词法与语法解析}
SQL解析器（基于 \texttt{lex\_sql.l} 和 \texttt{yacc\_sql.y}）负责识别别名。这包括处理 \texttt{AS} 关键字以及表名或列表达式后的直接别名。

在 \texttt{yacc\_sql.y} 中，相关规则会被定义来捕获表别名和列别名：
\begin{lstlisting}[language=SQL]
-- Conceptual YACC rules for table aliases
table_reference:
    table_name K_AS IDENTIFIER  { /* Store table name and alias */ }
  | table_name IDENTIFIER       { /* Store table name and alias (implicit) */ }
  | table_name                  { /* No alias */ }
  ;

-- Conceptual YACC rules for select item (column) aliases
select_expr:
    expr K_AS IDENTIFIER        { /* Store expression and its alias */ }
  | expr IDENTIFIER             { /* Store expression and its alias (implicit, if grammar allows) */ }
  | expr                        { /* No alias */ }
  ;
\end{lstlisting}
解析后，这些别名信息会被存储在抽象语法树 (AST) 节点中，例如 \texttt{ParsedSqlNode} 的派生类，如 \texttt{TableReferenceNode} 和 \texttt{SelectItemNode}。

\begin{lstlisting}[language=C++]
// Conceptual AST node for a table reference
// Potentially in src/observer/sql/parser/parsed_sql_node.h
class TableReferenceNode : public ParsedSqlNode {
public:
    std::string table_name_;
    std::string alias_; // Empty if no alias
    // ...
};

// Conceptual AST node for a select item
class SelectItemNode : public ParsedSqlNode {
public:
    ExpressionNode* expr_;
    std::string alias_; // Empty if no alias
    // ...
};
\end{lstlisting}

\paragraph{语义分析与名称解析}
在 \texttt{ResolveStage} 阶段，对AST中的别名进行处理和验证。
对于表别名：
\begin{itemize}
    \item 别名与实际表名被绑定。
    \item 在当前查询块（query block）的作用域内，表别名必须唯一。通常会使用一个上下文或符号表来跟踪当前作用域内已定义的表及其别名。
    \item 子查询可以引入与外部查询同名的表别名，因为它们拥有独立的作用域。
\end{itemize}
对于列别名：
\begin{itemize}
    \item 列别名与 \texttt{SELECT} 列表中的表达式相关联。
    \item 根据要求，列别名仅用于结果显示，无需进行唯一性检查或用于后续计算。
\end{itemize}
此阶段会将包含别名信息的 \texttt{ParsedSqlNode} 转换为内部的 \texttt{Stmt} 对象，例如 \texttt{SelectStmt}。

\begin{lstlisting}[language=C++]
// Conceptual structure for a resolved table reference
// Potentially in src/observer/sql/stmt/select_stmt.h or similar
struct ResolvedTableItem {
    std::string table_name_;    // Actual table name
    std::string alias_;         // Alias for this table in the current scope
    int table_id_;              // Internal ID for the table
    // ... methods to check for alias, get effective name
};

// Conceptual structure for a resolved select list item
struct ResolvedSelectItem {
    Expr* expr_;                // The expression being selected
    std::string alias_;         // Alias for this expression in the output
    std::string computed_name_; // Name to be used if no alias
    // ...
};

// In ResolveStage, when processing FROM clause:
// For each table_reference:
//   std::string alias = table_ref_node->alias_.empty() ? table_ref_node->table_name_ : table_ref_node->alias_;
//   if (current_scope_symbol_table.contains_table_alias(alias)) {
//     // Report error: duplicate table alias in the same query level
//   } else {
//     // Add (alias -> actual_table_name) to symbol_table
//     // Create ResolvedTableItem
//   }

// In ResolveStage, when processing SELECT list:
// For each select_item_node:
//   // Create ResolvedSelectItem, storing expr_ and alias_
//   // No complex validation for column alias as per requirements.
\end{lstlisting}

\paragraph{使用表别名进行列解析}
在查询处理过程中，特别是在解析列引用（例如 \texttt{alias.column}) 时，会使用表别名。语义分析器和后续的执行器需要通过表别名查找到其代表的实际表，然后才能定位到具体的列。

\begin{lstlisting}[language=C++]
// Conceptual ColumnRef resolution
// Potentially in src/observer/sql/resolver/resolve_visitor.cpp
// When resolving a ColumnRef node like "t1.colA":
//   string table_alias_from_ref = column_ref_node->table_name_; // "t1"
//   string column_name_from_ref = column_ref_node->column_name_; // "colA"
//
//   ResolvedTableItem* resolved_table = current_scope_symbol_table.find_table_by_alias(table_alias_from_ref);
//   if (!resolved_table) {
//     // Report error: unknown table alias
//   } else {
//     // Proceed to find column_name_from_ref in resolved_table->actual_table_name_ schema
//   }
\end{lstlisting}

\paragraph{结果生成与别名显示}
在 \texttt{ExecuteStage} 阶段，当查询结果被格式化输出时，会使用列别名。
\begin{itemize}
    \item 如果 \texttt{SELECT} 列表中的一个表达式指定了别名，则在结果集的列头中使用该别名。
    \item 如果没有指定别名，则可能使用表达式的文本表示或底层列名作为列头。
    \item 表别名通常不直接出现在最终用户的结果集中，但它们是内部处理字段引用的关键。
\end{itemize}
结果集对象（例如 \texttt{SqlResult}）需要能够存储并提供每一列的最终显示名称（即别名，如果存在）。

\begin{lstlisting}[language=C++]
// Conceptual part of SqlResult or result set formatter
// Potentially in src/observer/sql/executor/sql_result.h
class SqlResult {
public:
    // ...
    std::vector<std::string> column_names_; // Stores the display names for columns (aliases)
    std::vector<std::vector<Value>> rows_;
    // ...
    void set_column_names(const std::vector<ResolvedSelectItem>& select_items) {
        column_names_.clear();
        for (const auto& item : select_items) {
            if (!item.alias_.empty()) {
                column_names_.push_back(item.alias_);
            } else {
                column_names_.push_back(item.computed_name_); // Or expression string
            }
        }
    }
};

// During execution, after producing rows and before sending to client:
//   sql_result_object.set_column_names(resolved_select_list_items);
//   // Then populate rows and send the sql_result_object
\end{lstlisting}
通过这些机制，系统支持字段和表的别名功能，简化了复杂查询的编写，并允许用户定制查询结果的输出格式。

\subsection{数据分组功能实现}

数据分组功能允许对查询结果按照一个或多个字段进行分区，并对每个分区应用聚合函数，是数据分析的核心功能。其实现涉及SQL解析、语义分析、计划生成与执行等多个环节。

\paragraph{词法与语法解析}
SQL解析器（基于 \texttt{lex\_sql.l} 和 \texttt{yacc\_sql.y}）需识别 \texttt{GROUP BY} 和 \texttt{HAVING} 子句。
\begin{itemize}
    \item \texttt{GROUP BY} 子句指定用于分组的一个或多个表达式（通常是列名）。
    \item \texttt{HAVING} 子句指定应用于分组后结果的过滤条件，通常包含聚合函数。
\end{itemize}
在 \texttt{yacc\_sql.y} 中，会定义相应的语法规则来捕获这些子句。

\begin{lstlisting}[language=SQL]
-- Conceptual YACC rules for GROUP BY and HAVING
select_statement:
    SELECT select_list
    FROM table_references
    WHERE where_condition
    GROUP BY group_expression_list
    HAVING having_condition
    { /* ... */ }
  | SELECT select_list
    FROM table_references
    WHERE where_condition
    GROUP BY group_expression_list
    { /* ... */ }
  | -- other select forms
  ;

group_expression_list:
    expression
  | group_expression_list ',' expression
  ;
\end{lstlisting}
解析后，分组表达式列表和 \texttt{HAVING} 条件表达式会存储在抽象语法树 (AST) 节点中，例如附加到 \texttt{SelectStmtNode} 或类似的结构上。

\begin{lstlisting}[language=C++]
// Conceptual extension to ParsedSqlNode for SELECT statements
// Potentially in src/observer/sql/parser/parsed_sql_node.h
class SelectStmtNode : public ParsedSqlNode {
public:
    // ... other select components ...
    std::vector<ExpressionNode*> group_by_exprs_;
    ExpressionNode* having_clause_expr_; // Null if no HAVING clause
    // ...
};
\end{lstlisting}

\paragraph{语义分析与计划生成}
在 \texttt{ResolveStage}（语义分析阶段）：
\begin{itemize}
    \item 校验 \texttt{GROUP BY} 子句中的表达式。这些表达式通常必须是 \texttt{SELECT} 列表中非聚合表达式，或者是输入表的列。
    \item 解析 \texttt{SELECT} 列表和 \texttt{HAVING} 子句中的聚合函数（如 \texttt{COUNT}, \texttt{SUM}, \texttt{AVG} 等）和普通表达式。
    \item 确保 \texttt{HAVING} 子句中的表达式要么是聚合函数，要么出现在 \texttt{GROUP BY} 列表中。
\end{itemize}
在 \texttt{OptimizeStage}（优化阶段）：
\begin{itemize}
    \item 从 \texttt{SelectStmt} 生成逻辑计划。分组和聚合操作通常由逻辑算子如 \texttt{LogicalAggregate} 表示。
    \item 生成物理计划，如 \texttt{HashAggregatePhysicalOperator} 或 \texttt{SortAggregatePhysicalOperator}。 \texttt{HashAggregate} 通常更优，它使用哈希表来组织分组。
\end{itemize}

\begin{lstlisting}[language=C++]
// Conceptual SelectStmt after resolution (src/observer/sql/stmt/select_stmt.h)
class SelectStmt : public Stmt {
public:
    // ...
    std::vector<Expr*> group_by_exprs_;
    std::vector<AggFuncExpr*> aggregate_functions_; // Aggregates from SELECT and HAVING
    Expr* having_expr_; // Resolved HAVING clause expression
    // ...
};

// Conceptual AggregatePhysicalOperator (src/observer/sql/executor/aggregate_physical_operator.h)
class AggregatePhysicalOperator : public PhysicalOperator {
public:
    PhysicalOperator* child_;
    std::vector<Expr*> group_by_exprs_;
    std::vector<AggFuncExpr*> agg_exprs_;
    Expr* having_expr_;
    // Internal state for hash-based aggregation:
    // std::unordered_map<GroupKey, AggregateState> hash_table_;
    // ...
    AggregatePhysicalOperator(PhysicalOperator* child,
                              std::vector<Expr*> group_by_exprs,
                              std::vector<AggFuncExpr*> agg_exprs,
                              Expr* having_expr);
    bool next(Tuple* tuple) override;
private:
    void consume_input_and_build_hash_table();
    // Iterator for outputting groups:
    // std::unordered_map<GroupKey, AggregateState>::iterator output_iterator_;
    bool initialized_ = false;
};
\end{lstlisting}

\paragraph{聚合与分组执行}
\texttt{AggregatePhysicalOperator} 负责执行分组和聚合。
\begin{enumerate}
    \item 初始化：首次调用 \texttt{next()} 时，从其子算子拉取所有元组，并构建分组聚合的内部状态（例如哈希表）。
    \item 哈希表构建：
        \begin{itemize}
            \item 对于每个输入元组，计算其在 \texttt{group\_by\_exprs\_} 上的值，形成一个 \texttt{GroupKey}。
            \item 使用此 \texttt{GroupKey} 在哈希表中查找或创建条目。
            \item 更新与该 \texttt{GroupKey} 关联的 \texttt{AggregateState}（例如，对于 \texttt{SUM}，累加值；对于 \texttt{COUNT}，递增计数器）。
        \end{itemize}
    \item 结果产出：哈希表构建完毕后，遍历哈希表中的条目。对于每个条目（即每个组）：
        \begin{itemize}
            \item 计算最终的聚合函数值。
            \item （在此阶段或之后）应用 \texttt{HAVING} 子句过滤。
            \item 如果满足 \texttt{HAVING} 条件（或无 \texttt{HAVING} 子句），则构造输出元组（包含分组键值和聚合结果值）并返回。
        \end{itemize}
\end{enumerate}

\begin{lstlisting}[language=C++]
// Conceptual logic within AggregatePhysicalOperator::consume_input_and_build_hash_table()
// Tuple input_tuple;
// while (child_->next(&input_tuple)) {
//   GroupKey key;
//   for (Expr* group_expr : group_by_exprs_) {
//     key.add_value(group_expr->evaluate(&input_tuple));
//   }
//
//   AggregateState& state = hash_table_[key]; // operator[] creates if not exists
//   if (state.is_new()) { // First time seeing this group
//      state.initialize(agg_exprs_);
//   }
//   for (size_t i = 0; i < agg_exprs_.size(); ++i) {
//     Value val_to_aggregate = agg_exprs_[i]->get_argument_expr()->evaluate(&input_tuple);
//     state.update(i, val_to_aggregate); // Update i-th aggregate
//   }
// }

// Conceptual logic within AggregatePhysicalOperator::next() after hash table built
// while (output_iterator_ != hash_table_.end()) {
//    const GroupKey& group_key = output_iterator_->first;
//    AggregateState& agg_state = output_iterator_->second;
//    ++output_iterator_; // Move iterator for next call
//
//    Tuple output_tuple;
//    // Populate output_tuple with group_key values
//    // Populate output_tuple with final aggregate values from agg_state
//
//    if (having_expr_) {
//      // Evaluate having_expr_ using the context of the current group (group_key and final aggregates)
//      // This requires aggregates in having_expr_ to refer to the computed agg_state.
//      Value having_result = having_expr_->evaluate_with_aggregates(&output_tuple, &agg_state);
//      if (!having_result.is_true()) {
//        continue; // Skip this group
//      }
//    }
//    *tuple = output_tuple;
//    return true;
// }
// return false; // No more groups
\end{lstlisting}

\paragraph{HAVING 子句过滤}
\texttt{HAVING} 子句在所有行被分组并且聚合值计算完毕之后应用。它对每个分组应用条件过滤。如上述代码片段所示，\texttt{AggregatePhysicalOperator} 在产出每个分组的结果之前，会使用该分组的聚合值来评估 \texttt{HAVING} 条件。只有当条件评估为真时，该分组才会成为最终结果的一部分。由于聚合函数不能用于 \texttt{WHERE} 子句（\texttt{WHERE} 在分组前操作），\texttt{HAVING} 是对聚合结果进行过滤的标准方式。

\paragraph{NULL 值在分组中的处理}
当 \texttt{GROUP BY} 子句中的字段包含 \texttt{NULL} 值时，所有具有 \texttt{NULL} 值的行（在其他分组字段值也相同的情况下）被视为属于同一个组。即，在分组键的比较中，一个 \texttt{NULL} 值被认为等同于另一个 \texttt{NULL} 值。
在基于哈希的分组中，这意味着从分组表达式计算得到的 \texttt{Value} 对象在用作哈希键的一部分时，其哈希函数和等价比较需要能正确处理 \texttt{NULL\_TYPE}。例如，\texttt{Value::hash()} 应为 \texttt{NULL} 值产生一致的哈希码，并且 \texttt{Value::operator==()} 应认为两个 \texttt{NULL} 值相等（仅为分组目的）。

\begin{lstlisting}[language=C++]
// Conceptual Value class enhancements for grouping
// Potentially in src/observer/sql/expr/value.h
class Value {
public:
    // ...
    size_t hash_for_grouping() const {
        if (is_null()) {
            return 0; // Or some other constant hash for NULL
        }
        // ... actual hash calculation for non-null ...
    }

    bool equals_for_grouping(const Value& other) const {
        if (is_null() && other.is_null()) {
            return true;
        }
        if (is_null() || other.is_null()) {
            return false; // NULL not equal to non-NULL
        }
        return (*this == other); // Use existing equality for non-NULLs
    }
    // ...
};

// GroupKey would use these methods for its internal hash and equality
// class GroupKey {
//   std::vector<Value> key_values_;
//   // ...
//   struct Hasher {
//     std::size_t operator()(const GroupKey& gk) const {
//       std::size_t seed = 0;
//       for (const auto& val : gk.key_values_) {
//         seed ^= val.hash_for_grouping() + 0x9e3779b9 + (seed << 6) + (seed >> 2);
//       }
//       return seed;
//     }
//   };
//   bool operator==(const GroupKey& other) const {
//     if (key_values_.size() != other.key_values_.size()) return false;
//     for (size_t i = 0; i < key_values_.size(); ++i) {
//       if (!key_values_[i].equals_for_grouping(other.key_values_[i])) {
//         return false;
//       }
//     }
//     return true;
//   }
// };
\end{lstlisting}
通过这些机制，系统实现了对数据进行分组聚合，并支持使用 \texttt{HAVING} 子句对分组结果进行筛选，同时正确处理了分组键中的 \texttt{NULL} 值。

\subsection{复杂子查询功能实现}

复杂子查询，或称关联子查询（Correlated Subquery），其内部查询的执行依赖于外部查询的当前行数据。此特性使其能够表达更为复杂的查询逻辑，尤其在条件判断中涉及与外部查询行相关的聚合值时。

\paragraph{语法解析与AST表示}
SQL解析器（基于 \texttt{lex\_sql.l} 和 \texttt{yacc\_sql.y}）负责识别嵌套的 \texttt{SELECT} 语句。子查询可以出现在主查询的 \texttt{SELECT} 列表、\texttt{FROM} 子句（作为派生表）或 \texttt{WHERE}/\texttt{HAVING} 子句中。
当子查询出现在 \texttt{WHERE} 子句中，例如与 \texttt{IN}, \texttt{EXISTS}, \texttt{ANY}, \texttt{ALL} 等操作符结合，或作为比较表达式的一员时，其语法结构被解析并内嵌到主查询的AST节点中。

\begin{lstlisting}[language=SQL]
-- Conceptual YACC rule for a subquery in an expression
expression:
    // ... other expression forms
    | expression K_IN '(' select_statement ')' { /* Create subquery expression node */ }
    | K_EXISTS '(' select_statement ')'       { /* Create exists subquery node */ }
    | '(' select_statement ')'                { /* Scalar subquery or part of comparison */ }
    ;
\end{lstlisting}
解析后，子查询本身（一个完整的 \texttt{select\_statement} AST）会成为一个特殊类型的表达式节点 (\texttt{SubqueryExprNode}) 或类似结构的一部分，该节点嵌入到外部查询的AST中。

\begin{lstlisting}[language=C++]
// Conceptual AST node for a subquery expression
// Potentially in src/observer/sql/parser/parsed_sql_node.h or expr_node.h
class SubqueryExprNode : public ExpressionNode {
public:
    SelectStmtNode* subquery_ast_; // The AST of the inner SELECT statement
    SubqueryType type_; // e.g., SCALAR, IN, EXISTS, ANY, ALL
    // ...
};
\end{lstlisting}

\paragraph{语义分析与关联识别}
在 \texttt{ResolveStage}（语义分析阶段）：
\begin{itemize}
    \item 对子查询的AST进行独立的语义分析，但需要能够访问外部查询的作用域以解析关联列。
    \item 识别关联列：子查询中引用的列如果未在子查询自身的 \texttt{FROM} 子句中定义，则尝试在外部查询的 \texttt{FROM} 子句中寻找。如果找到，则该列被识别为关联列。
    \item 关联信息（例如，哪些外部列被内部查询引用）被记录在子查询的已解析表示中（例如，在转换后的 \texttt{SelectStmt} 对象中）。
\end{itemize}
子查询的 \texttt{SelectStmt} 对象会包含其自身的解析信息，并可能有一个指向外部查询上下文的引用或一个关联列表。

\begin{lstlisting}[language=C++]
// Conceptual SelectStmt (src/observer/sql/stmt/select_stmt.h) with correlation support
class SelectStmt : public Stmt {
public:
    // ... existing fields ...
    bool is_subquery_ = false;
    std::vector<CorrelatedColumnInfo> correlated_columns_; // Info about columns from outer query

    // For subqueries in expressions:
    // Expr* subquery_expr_node_ref; // Reference back to the SubqueryExprNode if needed
    // ...
};

// Conceptual SubqueryExpr (resolved expression node)
// src/observer/sql/expr/subquery_expr.h
class SubqueryExpr : public Expr {
public:
    SelectStmt* subquery_stmt_; // The resolved SelectStmt for the subquery
    // std::vector<Expr*> params_from_outer_query_; // Expressions providing values for correlated columns
    // ...
    Value evaluate(Tuple* outer_query_tuple) const override;
};
\end{lstlisting}

\paragraph{执行策略与优化}
关联子查询的执行对性能有显著影响。主要策略包括：
\begin{enumerate}
    \item \textbf{嵌套循环执行（Tuple-at-a-time）}：对外部查询的每一行，都执行一次内部子查询。内部子查询执行时，使用来自外部当前行的关联列的值。这是最直接的执行方式，但通常效率低下。
        \begin{itemize}
            \item 物理执行计划中可能使用 \texttt{NestedLoopJoinPhysicalOperator} 的变体或一个专门的 \texttt{ApplyPhysicalOperator} (或 \texttt{DependentExecutePhysicalOperator})。该算子为外部查询的每一行重新评估其右侧（子查询）计划。
        \end{itemize}
    \item \textbf{去关联（Decorrelation）}：在 \texttt{OptimizeStage}，通过 \texttt{Rewriter} 将关联子查询转换为等价的非关联子查询或连接操作。这是提高性能的关键。
        \begin{itemize}
            \item \texttt{EXISTS} 和 \texttt{IN} 子查询常可转换为半连接（Semi-Join）。
            \item 带有聚合的子查询可能通过引入分组的派生表并与之连接来进行去关联。
            \item 标量子查询（返回单行单列）若被关联，有时可通过引入外部连接（Outer Join）并结合窗口函数或聚合来处理。
        \end{itemize}
\end{enumerate}
如果无法完全去关联，执行器仍需支持参数化的子查询执行。

\begin{lstlisting}[language=C++]
// Conceptual ApplyPhysicalOperator (for nested loop execution of correlated subqueries)
// src/observer/sql/executor/apply_physical_operator.h
class ApplyPhysicalOperator : public PhysicalOperator {
public:
    PhysicalOperator* outer_child_;
    PhysicalPlan* inner_plan_template_; // Template for the subquery's plan
    // std::vector<std::pair<OuterColIdx, InnerParamIdx>> correlation_bindings_;

    ApplyPhysicalOperator(PhysicalOperator* outer_child, PhysicalPlan* inner_plan_template /*, bindings */);
    bool next(Tuple* tuple) override; // Combines outer tuple with subquery result

private:
    Tuple current_outer_tuple_;
    // PhysicalOperator* current_inner_executor_; // Instantiated from inner_plan_template_ with current_outer_tuple_ values
    bool outer_has_more_ = true;
    // Method to re-instantiate or re-parameterize current_inner_executor_
    // void rebind_inner_plan_with_params(const Tuple& outer_row_params);
};

// Conceptual Rewriter for decorrelation (e.g., ExistsToSemiJoinRewriter)
// src/observer/sql/optimizer/rewriter/decorrelation_rewriter.h
class DecorrelationRewriter : public Rewriter {
public:
    LogicalOperator* rewrite(LogicalOperator* op) override {
        // Traverse the logical plan
        // If a LogicalFilter with a SubqueryExpr (EXISTS type) is found:
        //   Try to convert it into a LogicalSemiJoinOperator
        //   This involves pulling up the subquery's FROM clause and merging predicates.
        // ... other decorrelation rules for IN, scalar subqueries etc. ...
        return op;
    }
};
\end{lstlisting}

\paragraph{处理带聚合的关联子查询}
当关联子查询包含聚合函数时（例如，\texttt{WHERE outer.col > (SELECT AVG(inner.col) FROM inner\_table WHERE inner.key = outer.key)})：
\begin{itemize}
    \item \textbf{嵌套执行}：对于外部的每一行，子查询被执行，其聚合函数（如 \texttt{AVG}）针对满足 \texttt{inner.key = outer.key} 条件的子集进行计算。
    \item \textbf{去关联}：
        \begin{enumerate}
            \item 创建一个派生表，该派生表对 \texttt{inner\_table} 按 \texttt{inner.key} 进行分组，并计算聚合函数（如 \texttt{AVG(inner.col)})。
            \item 将此派生表与外部查询的表使用 \texttt{outer.key = derived\_table.key} 进行连接。
            \item 原始的子查询条件变为对连接后派生表中的聚合结果列的比较。
        \end{enumerate}
        这通常需要一个 \texttt{LogicalAggregate} 算子和一个 \texttt{LogicalJoin} 算子。
\end{itemize}
优化器需要识别这种模式并应用相应的重写规则。

\begin{lstlisting}[language=C++]
// Conceptual transformation for correlated aggregate subquery:
// Outer Query: SELECT o.val FROM outer_table o WHERE o.price > (SELECT AVG(i.price) FROM inner_table i WHERE i.group_id = o.id)
//
// Step 1: Create a plan for the aggregate subquery part as a derived table
//   DerivedAggPlan:
//     LogicalAggregate (group_by: i.group_id, aggregate_expr: AVG(i.price) AS avg_price)
//       |
//     LogicalTableScan (inner_table AS i)
//
// Step 2: Rewrite the main query to join with this DerivedAggPlan
//   Rewritten Plan:
//     LogicalProject (o.val)
//       |
//     LogicalFilter (o.price > da.avg_price)
//       |
//     LogicalJoin (condition: o.id = da.group_id)  // Left Outer Join if subquery could return no rows for a group
//      /   \\
// LogicalTableScan (outer_table AS o)   (DerivedAggPlan AS da)

// This transformation would be performed by a specific Rewriter rule.
\end{lstlisting}
通过这些复杂的解析、语义分析、优化（特别是去关联）和执行机制，系统得以支持功能强大且对性能敏感的复杂子查询。

\subsection{大量数据的增删改查功能实现}

处理大量数据的增删改查 (CRUD) 操作是对数据库管理系统核心能力的考验，要求存储引擎、内存管理和执行器具备高效率和稳定性。本系统通过一系列优化和健壮的组件来支持这些操作。

\paragraph{DML语句解析与执行器分派}
标准的SQL \texttt{INSERT}, \texttt{UPDATE}, \texttt{DELETE} 语句由解析器（\texttt{yacc\_sql.y}, \texttt{lex\_sql.l}）识别，并转换为对应的AST节点 (\texttt{InsertNode}, \texttt{UpdateNode}, \texttt{DeleteNode})。在语义分析 (\texttt{ResolveStage}) 后，这些节点被转换为内部语句对象 (\texttt{InsertStmt}, \texttt{UpdateStmt}, \texttt{DeleteStmt})。随后，在执行阶段 (\texttt{ExecuteStage})，这些语句对象被分派给相应的命令执行器：\texttt{InsertExecutor}, \texttt{UpdateExecutor}, \texttt{DeleteExecutor}。

\begin{lstlisting}[language=C++]
// Conceptual structure for DML statement objects
// In src/observer/sql/stmt/insert_stmt.h
class InsertStmt : public DMLStmt {
public:
    std::string table_name_;
    std::vector<std::string> columns_; // Optional, for specified columns
    std::vector<std::vector<Expr*>> value_lists_; // Values for rows to insert
    // ...
};

// In src/observer/sql/stmt/update_stmt.h
class UpdateStmt : public DMLStmt {
public:
    std::string table_name_;
    std::vector<std::pair<std::string, Expr*>> set_clauses_; // Column to update and new value expr
    Expr* where_clause_; // Condition for rows to update
    // ...
};

// In src/observer/sql/stmt/delete_stmt.h
class DeleteStmt : public DMLStmt {
public:
    std::string table_name_;
    Expr* where_clause_; // Condition for rows to delete
    // ...
};

// Execution dispatch in ExecuteStage (conceptual)
// if (stmt->type_ == StmtType::INSERT) {
//   executor = new InsertExecutor(static_cast<InsertStmt*>(stmt), ...);
// } else if (stmt->type_ == StmtType::UPDATE) {
//   executor = new UpdateExecutor(static_cast<UpdateStmt*>(stmt), ...);
// } else if (stmt->type_ == StmtType::DELETE) {
//   executor = new DeleteExecutor(static_cast<DeleteStmt*>(stmt), ...);
// }
// executor->execute(&sql_result);
\end{lstlisting}

\paragraph{记录管理与物理存储交互}
DML执行器通过存储引擎的核心组件与物理存储进行交互，主要是记录管理器 (\texttt{RecordManager}) 和索引管理器 (\texttt{IndexManager})。
\begin{itemize}
    \item \textbf{INSERT}: \texttt{InsertExecutor} 准备好待插入的记录数据。对于每一条记录，调用 \texttt{RecordManager} 的接口（如 \texttt{insert\_record}）将其写入表的数据文件中。同时，如果表上有索引，需要调用 \texttt{IndexManager} 的接口（如 \texttt{insert\_entry}）为新记录在每个索引中插入相应的索引条目。
    \item \textbf{DELETE}: \texttt{DeleteExecutor} 首先通过执行 \texttt{WHERE} 子句（可能利用索引扫描）定位到要删除的记录。对于每一条符合条件的记录，获取其 \texttt{RID} (Record ID)。然后调用 \texttt{RecordManager} 的接口（如 \texttt{delete\_record}）删除记录，并调用 \texttt{IndexManager} 的接口（如 \texttt{delete\_entry}）从所有索引中删除对应的条目。
    \item \textbf{UPDATE}: \texttt{UpdateExecutor} 也首先定位到要更新的记录。对于每条记录，它读取旧值，构造新值，然后通过 \texttt{RecordManager} 更新记录 (\texttt{update\_record})。索引的更新可能较为复杂：如果被更新的字段是索引键的一部分，则需要先从索引中删除旧的索引条目，再插入新的索引条目；如果非索引字段更新，则索引条目可能无需改变。
\end{itemize}
所有这些操作都通过缓冲池管理器 (\texttt{BufferPoolManager}) 进行，以最小化磁盘I/O。

\begin{lstlisting}[language=C++]
// Conceptual RecordManager interface (src/observer/storage/record/record_manager.h)
class RecordManager {
public:
    // ...
    // Returns RID of inserted record
    RID insert_record(Table* table, const Record& record);
    bool delete_record(Table* table, const RID& rid);
    bool update_record(Table* table, const RID& rid, const Record& new_record);
    // ...
};

// Conceptual IndexManager interface (src/observer/storage/index/index_manager.h)
class IndexManager {
public:
    // ...
    // KeyValue could be a struct bundling key fields, Value is often RID
    bool insert_entry(Index* index, const KeyValue& key, const RID& rid);
    bool delete_entry(Index* index, const KeyValue& key, const RID& rid);
    // For updates, might be a combination of delete and insert if key changes
    // ...
};

// Conceptual UpdateExecutor logic
// Tuple old_tuple;
// RID rid_to_update;
// Plan to find rows (e.g., TableScan or IndexScan with where_clause_):
// while (scan_operator->next(&old_tuple, &rid_to_update)) {
//   Record old_record = convert_tuple_to_record(old_tuple);
//   Record new_record = old_record; // Start with a copy
//   // Apply set_clauses_ to new_record using values from old_tuple and expressions
//
//   // For each index on the table:
//   //   Extract old_key_values from old_record for that index
//   //   index_manager_->delete_entry(index, old_key_values, rid_to_update);
//
//   record_manager_->update_record(table_object, rid_to_update, new_record);
//
//   // For each index on the table (again, if key changed or for new values):
//   //   Extract new_key_values from new_record for that index
//   //   index_manager_->insert_entry(index, new_key_values, rid_to_update);
// }
\end{lstlisting}

\paragraph{缓冲池与内存管理}
处理大量数据时，缓冲池管理器 (\texttt{src/observer/storage/buffer/}) 的效率至关重要。它负责将磁盘页 (\texttt{Page}) 缓存到内存帧 (\texttt{Frame}) 中。
\begin{itemize}
    \item 页面请求：当记录或索引管理器需要访问一个页面时，它向缓冲池请求该页面。如果页面已在内存中，则直接返回；否则，缓冲池从磁盘加载页面，可能需要根据替换策略（如LRU）逐出一个旧页面。
    \item 脏页写回：被修改过的页面（脏页）由缓冲池管理，并在适当时机（如事务提交、页面被逐出或检查点）写回磁盘。
    \item 预取与批量操作：对于扫描大量数据（如全表扫描或大范围索引扫描），可以实现预取逻辑，提前将可能需要的页面加载到缓冲池。DML操作也可能受益于对页面的批量修改和延迟写回。
\end{itemize}
有效的缓冲池大小和替换策略对于减少大量DML操作期间的磁盘瓶颈至关重要。

\paragraph{事务、日志与并发控制}
为保证大量增删改操作的原子性和持久性，事务管理器 (\texttt{src/observer/storage/trx/}) 和日志管理器 (\texttt{src/observer/storage/clog/}) 扮演核心角色。
\begin{itemize}
    \item 每个DML操作都在一个事务 (\texttt{Trx}) 上下文中执行。
    \item 对数据和索引页的修改会生成日志记录 (\texttt{CLogRecord})，并由日志管理器通过预写日志 (WAL) 机制写入提交日志文件。这确保了即使发生故障，系统也能恢复到一致状态。
    \item 虽然并发控制 (\texttt{Trx} 中的实现) 可能是最小的，但在处理大量数据时，对共享数据结构的访问（如页面、索引节点）仍需基本的互斥保护，以防止数据损坏。
\end{itemize}
当对大量数据进行随机增删改时，事务的正确提交与回滚，以及日志的有效记录和必要时的恢复，是系统稳定性的基石。

\paragraph{数据校验与查询}
在对预先创建并填充数据的表执行一系列随机的增删改操作后，通过执行 \texttt{SELECT} 查询来校验数据的正确性和一致性。
\begin{itemize}
    \item 校验特定记录是否存在或已被正确删除/更新。
    \item 执行聚合查询（如 \texttt{COUNT(*)}, \texttt{SUM(column)})，并将结果与预期值比较，以验证操作的整体影响。
    \item 查询涉及索引的列，以确保索引与数据保持同步。
\end{itemize}
例如，可以设计一个测试脚本，生成大量的随机DML操作，并间歇性地运行校验查询。这有助于发现数据损坏、索引不一致或事务处理错误等问题。

\begin{lstlisting}[language=SQL]
-- Example validation queries after random DMLs
-- Assume table 'test_large_data' with columns 'id INT PRIMARY KEY, value INT, category VARCHAR(50)'

-- Check if a specific inserted/updated record has the correct value
SELECT value FROM test_large_data WHERE id = 12345;

-- Count total rows after many inserts and deletes
SELECT COUNT(*) FROM test_large_data;

-- Sum of values in a category after updates
SELECT SUM(value) FROM test_large_data WHERE category = 'TypeA';

-- Check if a deleted record is indeed gone
SELECT * FROM test_large_data WHERE id = 67890; -- Should return empty set if deleted
\end{lstlisting}
通过综合运用高效的执行器、优化的存储引擎组件（特别是记录、索引和缓冲池管理）以及严格的事务和日志机制，系统能够处理大量数据的增删改查操作，并通过后续查询校验其稳定性和可靠性。

\subsection{MVCC中的更新功能实现}

多版本并发控制 (MVCC) 机制通过为数据项保留多个版本来管理并发事务访问，从而提高数据库的并发性能，尤其是在读多写少的场景下。\texttt{UPDATE} 操作在MVCC中不是原地修改，而是创建数据的新版本。

\paragraph{MVCC更新概述与行版本结构}
在MVCC模型中，当一个事务更新一行数据时，它实际上会创建一个该行的新版本，而旧版本则被保留（至少在一段时间内），以供其他可能正在读取旧数据的并发事务使用。为了支持这种机制，每一行记录 (\texttt{Record}) 除了包含实际数据外，还需要额外的版本控制信息。

\begin{lstlisting}[language=C++]
// Conceptual structure for a versioned Record
// Potentially in src/observer/storage/record/record.h
class Record {
public:
    // ... actual data fields ...
    char* data_; // Points to the actual row data

    // MVCC-specific header information, could be part of data_ or separate
    TransactionId creator_trx_id_; // ID of the transaction that created this version
    TransactionId deleter_trx_id_; // ID of the transaction that "deleted" or superseded this version
                                 // (e.g., MAX_TRX_ID if current, or the ID of the updating/deleting Trx)
    RowId previous_version_ptr_; // Pointer/RID to the previous version of this logical row (for history/rollback)

    // ... methods to access data and MVCC headers ...
    bool is_visible(TransactionSnapshot* snapshot) const {
        // A version is visible if:
        // 1. Its creator_trx_id_ is committed and part of the snapshot's past.
        // 2. Its deleter_trx_id_ is either not set (e.g., MAX_TRX_ID indicating current)
        //    OR its deleter_trx_id_ is not committed or is part of the snapshot's future.
        // Or, if creator_trx_id_ is the current transaction's ID.
        if (creator_trx_id_ == snapshot->get_current_trx_id()) {
            return true; // Visible to its own transaction
        }
        if (!snapshot->is_committed(creator_trx_id_) || creator_trx_id_ > snapshot->get_max_visible_trx_id()) {
            return false;
        }
        if (deleter_trx_id_ != MAX_TRX_ID) { // Assuming MAX_TRX_ID means not deleted
            if (snapshot->is_committed(deleter_trx_id_) && deleter_trx_id_ <= snapshot->get_max_visible_trx_id()) {
                 return false; // Deleted by a committed transaction visible in this snapshot
            }
        }
        return true;
    }
};
\end{lstlisting}
\texttt{TransactionSnapshot} 对象封装了特定事务的可见性视图，通常包含该事务启动时的活动事务列表和已提交事务的上限ID。

\paragraph{UPDATE语句处理与版本定位}
\texttt{UPDATE} 语句首先由SQL解析层处理，生成 \texttt{UpdateStmt} 对象。在执行阶段，\texttt{UpdateExecutor} 负责处理该语句。
\begin{enumerate}
    \item \textbf{定位行}：执行器首先根据 \texttt{WHERE} 子句扫描表（可能通过索引）以找到匹配的行。
    \item \textbf{版本选择}：对于每一条物理上匹配的行，需要找到对当前事务可见的最新版本。这涉及到遍历版本链（如果存在多版本）并使用当前事务的 \texttt{TransactionSnapshot} 调用每个版本的 \texttt{is\_visible()} 方法。
\end{enumerate}

\paragraph{新版本创建与旧版本标记}
当事务 \(T_U\)（具有事务ID \texttt{trx\_id\_U}）更新一个可见的旧版本 \(V_{old}\)（由事务 \(T_C\) 创建）时：
\begin{enumerate}
    \item \textbf{复制与修改}：复制 \(V_{old}\) 的数据内容，并根据 \texttt{UPDATE} 语句的 \texttt{SET} 子句修改数据，形成新数据 \(D_{new}\)。
    \item \textbf{创建新版本} \(V_{new}\)：使用 \(D_{new}\) 创建一条新的记录版本。
        \begin{itemize}
            \item \(V_{new}\).\texttt{creator\_trx\_id\_} 设置为 \texttt{trx\_id\_U}。
            \item \(V_{new}\).\texttt{deleter\_trx\_id\_} 初始化为未删除状态（例如，一个特殊的最大事务ID值）。
            \item \(V_{new}\).\texttt{previous\_version\_ptr\_} 指向 \(V_{old}\) 的物理位置 (\texttt{RID})。
        \end{itemize}
    \item \textbf{标记旧版本}：将 \(V_{old}\).\texttt{deleter\_trx\_id\_} 设置为 \texttt{trx\_id\_U}。这逻辑上"删除"了 \(V_{old}\)，使其对于后续启动的事务（或当前事务的未来快照，一旦 \(T_U\) 提交）不再可见。
    \item \textbf{存储新版本}：将 \(V_{new}\) 插入到表中。这可能意味着在新的页面或槽位存储。
\end{enumerate}

\begin{lstlisting}[language=C++]
// Conceptual logic in UpdateExecutor for a single row
// RID old_version_rid = ...; // RID of the visible old version found
// Record old_version_data = record_manager_->read_record(table, old_version_rid);
// Transaction* current_trx = session_manager_->get_current_transaction();
// TransactionId current_trx_id = current_trx->get_id();

// Check for write-write conflict (simplified: if old version was updated by another committed trx after our snapshot)
// This check depends on the isolation level, e.g., Snapshot Isolation's "first-committer-wins".
// if (old_version_data.deleter_trx_id_ != MAX_TRX_ID &&
//     transaction_manager_->is_committed(old_version_data.deleter_trx_id_) &&
//     old_version_data.deleter_trx_id_ > current_trx->get_snapshot()->get_max_visible_trx_id()) {
//    // Conflict: row was updated and committed by another transaction after we read it.
//    // Abort current_trx.
//    // return TRANSACTION_ABORT_ERROR;
// }


// Create new version
Record new_version_data = old_version_data; // Copy structure/data
// Apply SET clauses to new_version_data.data_
new_version_data.creator_trx_id_ = current_trx_id;
new_version_data.deleter_trx_id_ = MAX_TRX_ID; // Mark as currently newest
// new_version_data.previous_version_ptr_ = old_version_rid; // Link back

// Insert the new version
RID new_version_rid = record_manager_->insert_record_version(table, new_version_data);
if (!new_version_rid.is_valid()) { /* Handle insertion failure */ }

// Mark old version as deleted by current transaction
bool success_mark_old = record_manager_->mark_record_version_deleted(table, old_version_rid, current_trx_id);
if (!success_mark_old) { /* Handle failure, potentially rollback new version insertion */ }

// Update indexes:
//   For each index:
//     Extract old_key from old_version_data
//     index_manager_->delete_mvcc_entry(index, old_key, old_version_rid); // Or mark index entry versioned
//     Extract new_key from new_version_data
//     index_manager_->insert_mvcc_entry(index, new_key, new_version_rid);
\end{lstlisting}

\paragraph{索引更新}
索引在MVCC环境中必须正确地指向对事务可见的行版本，或者索引本身也需要版本化。
\begin{itemize}
    \item 如果索引键未被修改：索引条目可能仍指向旧版本的物理位置，但访问时需要通过版本链找到可见版本。或者，索引直接指向逻辑行的一个"头"版本指针，该指针总是被更新为最新提交版本的位置。
    \item 如果索引键被修改：旧的索引条目需要被逻辑删除（关联到其行版本的 \texttt{deleter\_trx\_id\_}），新的索引条目（指向新行版本 \(V_{new}\)）需要被插入。这两个索引操作也必须是原子的。
\end{itemize}
MiniOB中B+树索引 (\texttt{BPlusTreeIndex}) 的实现需要调整以适应多版本行。

\paragraph{事务提交与可见性}
一个事务 (\(T_U\)) 对数据所做的更新（即创建的新版本 \(V_{new}\) 和对旧版本 \(V_{old}\) 的标记）在 \(T_U\) 提交之前，对其他事务是不可见的。只有当 \(T_U\) 成功提交后，其 \texttt{creator\_trx\_id\_} 和 \texttt{deleter\_trx\_id\_} 标记才会对后续创建快照的事务生效。事务管理器 (\texttt{src/observer/storage/trx/}) 负责协调提交过程。

\paragraph{并发更新与冲突}
当多个事务并发更新同一逻辑行时，MVCC机制（通常结合特定的隔离级别如快照隔离）会处理冲突。
\begin{itemize}
    \item \textbf{读已提交 (Read Committed)}：事务总是读取最新的已提交版本。更新时，通常采用锁机制（如行锁）锁定要更新的最新提交版本，防止其他事务同时更新。
    \item \textbf{快照隔离 (Snapshot Isolation)}：事务在其快照内操作。如果事务 \(T_1\) 尝试更新一行，而该行已被另一并发已提交事务 \(T_2\) 在 \(T_1\) 的快照创建之后修改过，则 \(T_1\) 通常会因写冲突（write-write conflict）而中止（first-committer-wins 策略）。
\end{itemize}
MiniOB的事务管理器需要实现相应的冲突检测和解决逻辑。

\paragraph{垃圾回收（简述）}
随着更新操作的进行，会产生大量不再对任何活动事务或未来事务可见的旧行版本。这些"死"版本占用的空间需要通过垃圾回收（GC）过程来回收。GC机制需要安全地识别并移除这些死版本，同时不影响并发事务的正确性。如README所述，这部分在MiniOB中尚不完善。

通过上述机制，即使实现不完整，MiniOB也为MVCC的更新操作奠定了基础框架，核心在于行版本控制、新旧版本的原子性转换以及事务快照的可见性规则。

\subsection{大数据量排序功能实现}

\paragraph{概述与挑战}
处理大数据量的排序，特别是当源数据或中间结果（如多表笛卡尔积）超出可用主存容量时，需要采用外排序（External Sorting）策略。在内存有限的情况下，对大规模数据进行排序，同时满足快速返回首条记录和总体执行时间的要求，是对数据库查询优化器和执行引擎的关键考验。笛卡尔积操作可能产生远超原始表大小的中间结果集，这进一步加剧了排序的内存和I/O压力。

\paragraph{SQL解析与计划生成}
SQL查询中的 \texttt{ORDER BY} 子句由词法分析器 (\texttt{lex\_sql.l}) 和语法分析器 (\texttt{yacc\_sql.y}) 识别。语法规则捕获排序表达式及可选的排序方向 (\texttt{ASC} 或 \texttt{DESC})。
\begin{lstlisting}[language=SQL]
-- Conceptual YACC rule for ORDER BY clause
select_statement:
    SELECT select_list
    FROM table_references
    WHERE_clause_opt
    GROUP_BY_clause_opt
    HAVING_clause_opt
    ORDER_BY_clause_opt
    { /* Construct SelectStmtNode with all parts */ }
  ;

ORDER_BY_clause_opt:
    /* empty */ { $$ = nullptr; }
  | K_ORDER K_BY order_expression_list { /* Pass list to SelectStmtNode */ }
  ;

order_expression_list:
    order_expression sort_direction_opt
        { /* Create list with one item */ }
  | order_expression_list ',' order_expression sort_direction_opt
        { /* Append to list */ }
  ;

sort_direction_opt:
    /* empty */ { /* Default to ASC */ }
  | K_ASC
  | K_DESC
  ;

order_expression:
    expression { /* Represents a sort key */ }
  ;
\end{lstlisting}
解析后，排序表达式及其方向存储在抽象语法树 (AST) 节点中，如 \texttt{SelectStmtNode} 的一个成员。在语义分析阶段 (\texttt{ResolveStage})，这些排序表达式被解析和验证，转换为内部表示，例如 \texttt{SelectStmt} 对象中的 \texttt{OrderByItem} 结构列表。优化阶段 (\texttt{OptimizeStage}) 根据这些信息以及可用统计数据和代价模型（如果存在），生成包含排序操作的物理执行计划。这通常涉及到引入一个 \texttt{SortPhysicalOperator} (或 \texttt{OrderPhysicalOperator})。

\begin{lstlisting}[language=C++]
// Conceptual OrderByItem in SelectStmt (src/observer/sql/stmt/select_stmt.h)
struct OrderByItem {
    Expr* expr_;      // The expression to sort by (e.g., ColumnRefExpr)
    bool is_asc_;     // True for ASC, false for DESC
    // DBMS might also include NullSortOrder (NULLS FIRST/LAST) here
};

class SelectStmt : public Stmt {
public:
    // ... other members like select_list_, from_clause_, where_clause_ ...
    std::vector<OrderByItem> order_by_items_;
    // ...
};

// Conceptual SortPhysicalOperator (src/observer/sql/executor/sort_physical_operator.h)
// This operator would typically be responsible for external sorting if data is large.
class SortPhysicalOperator : public PhysicalOperator {
public:
    PhysicalOperator* child_;              // The operator producing data to be sorted
    std::vector<OrderByItem> sort_key_defs_;// Definitions of sort keys from SelectStmt
    // Internal state for sorting:
    // std::vector<Tuple> tuple_buffer_; // Buffer for tuples to be sorted in memory (a run)
    // std::vector<std::unique_ptr<File>> temp_run_files_; // For external sort runs
    // std::priority_queue<MergeRunTuple> merge_heap_; // For k-way merge
    // bool input_fully_consumed_ = false;
    // bool runs_generated_ = false;
    // bool merger_initialized_ = false;
    // size_t current_output_index_from_buffer_ = 0; // For in-memory sort output

    SortPhysicalOperator(PhysicalOperator* child, const std::vector<OrderByItem>& sort_key_defs);
    bool next(Tuple* tuple) override; // Fetches the next sorted tuple
private:
    // void generate_runs_externally(); // Phase 1 of external sort
    // void initialize_multiway_merge(); // Prepare for Phase 2
    // bool merge_next_tuple(Tuple* tuple); // Get next tuple from k-way merge
    // void sort_buffer_in_memory();
    // int compare_tuples(const Tuple& t1, const Tuple& t2) const;
};
\end{lstlisting}

\paragraph{外排序算法：生成初始顺串 (Runs)}
当子执行器（例如，处理笛卡尔积的 \texttt{NestedLoopJoinPhysicalOperator} 或 \texttt{HashJoinPhysicalOperator} 后续可能跟着一个产生大量行的投影操作）输出的数据量预计或实际超出预设的内存排序阈值时，\texttt{SortPhysicalOperator} 采用外排序算法。
\begin{enumerate}
    \item \textbf{内存缓冲与排序}：从子执行器逐一拉取元组 (\texttt{Tuple})，并将其存入一个专用的内存缓冲区。
    \item 当缓冲区满或子执行器无更多数据时，对缓冲区内的元组集合根据 \texttt{order\_by\_items\_} 中定义的排序键和方向进行内存排序。这可以使用标准的排序算法，如 \texttt{std::sort}，辅以一个自定义的比较函数，该函数能处理多列排序、不同数据类型及NULL值。
    \item \textbf{顺串写盘}：将内存中排序好的元组序列（称为一个"顺串"或"run"）写入一个临时的磁盘文件。
    \item 清空内存缓冲区，重复上述过程，直到所有来自子执行器的输入元组都被处理完毕。这将产生多个存储在磁盘上的、各自内部有序的临时顺串文件。
\end{enumerate}
此过程依赖缓冲池管理器 (\texttt{src/observer/storage/buffer/}) 高效地管理临时文件的页面创建、写入和后续读取。临时文件的I/O性能对整体排序速度有显著影响。

\begin{lstlisting}[language=C++]
// Conceptual comparison function used by std::sort or similar
// int SortPhysicalOperator::compare_tuples(const Tuple& t1, const Tuple& t2) const {
//   for (const auto& key_def : sort_key_defs_) {
//     Value v1 = key_def.expr_->evaluate_no_throw(&t1); // Evaluate sort key for t1
//     Value v2 = key_def.expr_->evaluate_no_throw(&t2); // Evaluate sort key for t2
//
//     // Handle NULLs according to SQL standard or system default (e.g., NULLS LAST)
//     if (v1.is_null() && v2.is_null()) continue;
//     if (v1.is_null()) return key_def.is_asc_ ? 1 : -1; // Or based on NULLS FIRST/LAST
//     if (v2.is_null()) return key_def.is_asc_ ? -1 : 1; // Or based on NULLS FIRST/LAST
//
//     int cmp_result = v1.compare(v2); // Value::compare handles type-specific comparison
//     if (cmp_result != 0) {
//       return key_def.is_asc_ ? cmp_result : -cmp_result;
//     }
//   }
//   return 0; // Tuples are equal with respect to all sort keys
// }

// Conceptual run generation (inside SortPhysicalOperator)
// void SortPhysicalOperator::generate_runs_externally() {
//   size_t sort_buffer_capacity_bytes = get_config_param("sort_buffer_size_bytes");
//   Tuple input_tuple;
//   while (child_->next(&input_tuple)) { // Fetch from child operator
//     if (tuple_buffer_current_size_bytes_ + input_tuple.get_size() > sort_buffer_capacity_bytes && !tuple_buffer_.empty()) {
//       // Sort current buffer
//       std::sort(tuple_buffer_.begin(), tuple_buffer_.end(),
//                 [this](const Tuple& a, const Tuple& b) { return compare_tuples(a, b) < 0; });
//       // Write sorted run to a new temporary file
//       temp_run_files_.push_back(write_run_to_temp_file(tuple_buffer_));
//       tuple_buffer_.clear();
//       tuple_buffer_current_size_bytes_ = 0;
//     }
//     tuple_buffer_.push_back(input_tuple.deep_copy()); // Store a copy
//     tuple_buffer_current_size_bytes_ += input_tuple.get_size();
//   }
//   // Sort and write the last partially filled run, if any
//   if (!tuple_buffer_.empty()) {
//     std::sort(tuple_buffer_.begin(), tuple_buffer_.end(), /* ... */);
//     temp_run_files_.push_back(write_run_to_temp_file(tuple_buffer_));
//     tuple_buffer_.clear();
//   }
//   input_fully_consumed_ = true;
//   runs_generated_ = true;
// }
\end{lstlisting}
\texttt{Tuple} 的比较逻辑必须正确处理不同数据类型和排序方向（ASC/DESC）。对 \texttt{NULL} 值的排序行为（例如，SQL标准允许指定 \texttt{NULLS FIRST} 或 \texttt{NULLS LAST}，MiniOB基础实现可能采用固定策略如 \texttt{NULLS LAST}）也在此比较函数中实现。

\paragraph{多路归并 (K-Way Merge)}
一旦所有初始顺串在磁盘上生成完毕，进入归并阶段，将这些有序的顺串合并成一个单一的、全局有序的结果流。
\begin{enumerate}
    \item \textbf{初始化归并结构}：为每个顺串文件打开一个读取器（通常带有输入缓冲区以优化磁盘读取）。使用一个最小优先队列（min-priority-queue 或 min-heap）来管理来自所有活动顺串的下一个最小元组。优先队列中的每个元素通常包含元组本身及其来源顺串的标识。
    \item \textbf{归并过程}：
        \begin{itemize}
            \item 从每个顺串读取其第一个元组，并将其插入优先队列。比较操作基于与初始排序阶段相同的排序键和逻辑。
            \item 反复从优先队列顶部抽取具有最小（或最大，对于DESC排序）排序键的元组。此元组即为下一个全局有序的元组。
            \item 将抽取的元组传递给上游算子（或直接作为结果）。
            \item 从该元组的来源顺串读取下一个元组。如果该顺串仍有数据，则将新读取的元组插入优先队列。若来源顺串已耗尽，则关闭其读取器。
            \item 持续此过程，直到所有顺串都已耗尽且优先队列为空。
        \end{itemize}
\end{enumerate}
归并的扇入度（即同时归并的顺串数量 K）受限于可用内存，因为需要为每个顺串维护一个输入缓冲区，并且优先队列本身也占用内存。如果初始顺串数量非常大，可能需要多轮归并（即先归并小子集的顺串，生成更长但数量更少的中间顺串，再对这些中间顺串进行归并）。

\begin{lstlisting}[language=C++]
// Conceptual structure for items in the merge priority queue
// struct MergeRunTuple {
//   Tuple tuple;
//   int run_index; // Identifier for the run this tuple came from
//   // Custom comparator for the priority queue
//   bool operator>(const MergeRunTuple& other) const {
//     // Comparison logic based on SortPhysicalOperator::compare_tuples
//     // Ensure it's a min-heap, so a > b means a has lower priority if a should come after b
//     return SortPhysicalOperator::compare_tuples(tuple, other.tuple) > 0;
//   }
// };

// Conceptual merge initialization (inside SortPhysicalOperator)
// void SortPhysicalOperator::initialize_multiway_merge() {
//   for (int i = 0; i < temp_run_files_.size(); ++i) {
//     RunFileReader* reader = temp_run_files_[i]->create_reader(); // Manages buffered reading
//     Tuple t;
//     if (reader->next_tuple(&t)) { // Read first tuple from run i
//       merge_heap_.push({t.deep_copy(), i, reader});
//     } else { /* Handle potentially empty run, though unlikely */ }
//   }
//   merger_initialized_ = true;
// }

// Conceptual next tuple from merger (inside SortPhysicalOperator::next)
// bool SortPhysicalOperator::merge_next_tuple(Tuple* output_tuple) {
//   if (merge_heap_.empty()) {
//     return false; // All runs merged, no more data
//   }
//
//   MergeHeapEntry top_entry = merge_heap_.top();
//   merge_heap_.pop();
//   *output_tuple = top_entry.tuple; // Output this tuple (transfer ownership or copy)
//
//   RunFileReader* source_reader = top_entry.reader;
//   Tuple next_t;
//   if (source_reader->next_tuple(&next_t)) { // Read next tuple from the same run
//     merge_heap_.push({next_t.deep_copy(), top_entry.run_index, source_reader});
//   } else {
//     // source_reader->close(); delete source_reader;
//     // temp_run_files_[top_entry.run_index]->mark_for_deletion();
//   }
//   return true;
// }
\end{lstlisting}

\paragraph{性能考量与优化策略}
针对大数据量排序，特别是笛卡尔积后的排序场景，需要考虑以下性能因素和优化：
\begin{itemize}
    \item \textbf{内存分配}：为初始顺串生成阶段的排序缓冲区和归并阶段的输入缓冲区分配足够的内存至关重要。更大的初始排序缓冲区可以产生更少、但更长的顺串，从而减少归并阶段的I/O次数和归并趟数。
    \item \textbf{快速首行输出}：为满足"第一条数据输出要在10秒以内"的要求，一旦第一个内存排序的顺串生成并写入磁盘，且归并阶段已初始化并从每个（或首批）顺串中读取了至少一个元组到优先队列，则第一个全局最小（或最大）元组即可被确定并输出。这个过程必须高效执行。如果数据量相对较小，或者排序键上有可利用的索引（通常不适用于笛卡尔积后的任意字段排序），优化器可能选择不进行外排序的策略。
    \item \textbf{临时文件管理}：使用高效的临时文件I/O（例如，通过缓冲池管理器进行异步读写，使用裸设备或特定文件系统优化），并在排序完成后及时清理临时文件。
    \item \textbf{数据存放与拉取}：题目中提及的"优化数据的存放和拉取"在排序上下文中可能指：
        \begin{itemize}
            \item 如果上游操作（如连接）可以流式地、按某种有利于后续排序的顺序（例如，部分按主排序键有序）生成元组，可以提高初始顺串的长度或质量。
            \item 在从子执行器拉取数据和向临时文件写数据，以及从临时文件读数据进行归并时，采用合适的预取（prefetching）和缓冲（buffering）策略，以平滑I/O操作，减少等待。
        \end{itemize}
    \item \textbf{CPU与I/O并行}：通过流水线化或多线程技术，尝试重叠CPU密集型任务（如元组比较、优先队列操作）与I/O密集型任务（读写顺串文件）。
    \item \textbf{笛卡尔积处理}：对于多表笛卡尔积，其结果集可能非常庞大（例如，三张各20行的表产生 \(20^3 = 8000\) 行，四张则为 \(20^4 = 160000\) 行）。如果每行元组较大，这足以触发外排序。关键在于流式处理笛卡尔积的输出，直接送入排序算子的初始顺串生成阶段，避免物化整个中间笛卡尔积结果。
\end{itemize}
通过上述外排序机制及相关优化，系统能够在有限内存条件下处理由大量数据（特别是笛卡尔积）产生的排序需求，力求在指定的时间限制内完成操作并快速响应首行数据。

\subsection{TEXT类型字段实现与超长记录存储}

\paragraph{概述}
为满足存储如网页等超大数据的需求，引入TEXT类型字段。参考MySQL的实现，TEXT字段最大长度设定为65535字节。超出此长度的插入操作将报错。此功能的实现不仅涉及SQL语法解析层面，更核心的是对存储引擎中记录管理器(\texttt{RecordManager})的扩展，以支持记录跨越单个数据页存储。

\paragraph{SQL解析与类型定义}
在SQL层面，DDL语句如 \texttt{CREATE TABLE} 需要能够解析并接受 \texttt{TEXT} 作为字段类型。这要求在词法分析器 (\texttt{lex\_sql.l}) 和语法分析器 (\texttt{yacc\_sql.y}) 中添加对 \texttt{TEXT} 关键字的识别，并将其映射到内部的类型系统中。

\begin{lstlisting}[language=SQL]
-- Conceptual YACC rule for column definition in CREATE TABLE
column_definition:
    column_name K_TEXT { /* Record column as TEXT type */ }
  | column_name K_VARCHAR \'(\' L_INT \')\' { /* Existing VARCHAR */ }
  | column_name K_INT { /* Existing INT */ }
  | -- other type definitions
  ;
\end{lstlisting}
在系统的元数据管理部分 (\texttt{src/observer/storage/table/})，\texttt{TableMeta} 或类似的结构中，需要增加对 \texttt{TEXT} 类型的表示。这可能涉及到扩展现有的字段类型枚举或类。

\begin{lstlisting}[language=C++]
// Conceptual extension to field type enum or class
// Potentially in src/observer/storage/table/field_meta.h or common/data_type.h
enum class DataType {
    INVALID,
    INT,
    VARCHAR,
    TEXT, // New type
    // ... other types
};

class FieldMeta {
public:
    std::string name_;
    DataType type_;
    int length_; // For VARCHAR, length is char count; for TEXT, could be max bytes (65535)
    // ...
};
\end{lstlisting}
在插入或更新操作时，需要校验 \texttt{TEXT} 字段的输入数据长度。如果提供的字节数超过65535，则应在语义分析或执行阶段拒绝该操作并返回错误。

\paragraph{记录管理器扩展：处理超长记录}
核心挑战在于存储引擎的记录管理器 (\texttt{src/observer/storage/record/}) 如何处理可能超过单个数据页 (\texttt{Page}) 大小的 \texttt{TEXT} 字段。这通常采用将大对象 (LOB - Large Object) 分片存储的策略。
\begin{enumerate}
    \item \textbf{记录结构调整}：对于包含 \texttt{TEXT} 字段的记录，其在主数据页上的存储可能只包含该字段的一个"描述符"或"指针"，而不是完整的 \texttt{TEXT} 内容。这个描述符指向实际存储 \texttt{TEXT} 数据的辅助页（或页链）。
    \item \textbf{辅助页/溢出页 (Overflow Pages)}：当一个 \texttt{TEXT} 字段的数据过大，无法内联存储在主记录的页内时，其内容会被存储在一个或多个专门的辅助页中。这些辅助页可以链接起来形成一个链表或树状结构来存储整个 \texttt{TEXT} 对象。
    \item \textbf{分片与重组}：
        \begin{itemize}
            \item \textbf{写入时}：当插入或更新包含大型 \texttt{TEXT} 字段的记录时，\texttt{RecordManager} 首先尝试内联存储。如果超过页内可用空间或特定阈值，则将 \texttt{TEXT} 数据分片。第一个分片可能部分存储在主记录页，其余分片存储在分配的辅助页中。每个辅助页除了存储数据分片外，还需要存储指向下一个分片所在页的指针 (\texttt{PageNum})。
            \item \textbf{读取时}：当读取包含 \texttt{TEXT} 字段的记录时，\texttt{RecordManager} 首先读取主记录页。如果发现 \texttt{TEXT} 字段是指向辅助页的描述符，则根据描述符逐个读取辅助页，并将所有分片按序重组为完整的 \texttt{TEXT} 字段内容。
        \end{itemize}
\end{enumerate}
这要求对 \texttt{RecordPageHandler} 和 \texttt{RecordFileHandler} 进行修改，使其能够识别和管理这种跨页记录。

\begin{lstlisting}[language=C++]
// Conceptual Record structure supporting out-of-page TEXT fields
// Potentially in src/observer/storage/record/record.h
class Record {
public:
    // ... existing members ...
    char* data_; // Points to the in-page part of the record

    // For a TEXT field within the record\'s data structure (represented in data_):
    // It might be a struct like:
    // struct TextFieldPtr {
    //   uint32_t actual_length; // Full length of the TEXT data
    //   PageNum first_overflow_page_id; // PageID of the first overflow page, or NULL_PAGE_ID if fully in-row
    //   uint16_t in_row_length; // Length of data stored in-row (if any)
    //   char in_row_data[...]; // Actual in-row data fragment
    // };
    // The RecordManager would interpret this structure.
    // ...
};

// Conceptual modifications to RecordManager
// Potentially in src/observer/storage/record/record_manager.cpp
class RecordManager {
public:
    // When inserting/updating a record with TEXT data:
    // RID insert_record_with_lob(Table* table, const Record& record_header, const std::map<int, std::string>& lob_fields) {
    //   // 1. Serialize fixed-length parts and small variable-length parts of the record.
    //   // 2. For each large TEXT field in lob_fields:
    //   //    char* text_data = lob_fields[field_idx].data();
    //   //    uint32_t text_len = lob_fields[field_idx].length();
    //   //    if (text_len > MAX_TEXT_LENGTH) { /* return error */ }
    //   //
    //   //    PageNum current_page_id = main_record_page_id;
    //   //    uint16_t slot_num_for_lob_chunk;
    //   //    uint32_t remaining_len = text_len;
    //   //    uint32_t offset_in_text = 0;
    //   //
    //   //    // Try to fit some initial part in the main record page or a dedicated slot.
    //   //    // For the rest, allocate overflow pages.
    //   //    PageNum prev_overflow_page = NULL_PAGE_ID;
    //   //    PageNum first_overflow_page = NULL_PAGE_ID;
    //   //
    //   //    while (remaining_len > 0) {
    //   //      Page* lob_page;
    //   //      if (first_overflow_page == NULL_PAGE_ID && can_store_first_chunk_inline) {
    //   //         // Store first chunk in main page or an already allocated page.
    //   //         // first_overflow_page might remain NULL_PAGE_ID or point to next.
    //   //      } else {
    //   //         lob_page = buffer_pool_manager_->new_page(table->get_lob_file_id()); // Or a dedicated LOB file
    //   //         if (first_overflow_page == NULL_PAGE_ID) first_overflow_page = lob_page->get_page_id();
    //   //         if (prev_overflow_page != NULL_PAGE_ID) {
    //   //           // Update previous LOB page to point to this new lob_page.
    //   //           // RecordPageHandler::set_next_lob_page(prev_lob_page_ptr, lob_page->get_page_id());
    //   //         }
    //   //         prev_overflow_page = lob_page->get_page_id();
    //   //      }
    //   //      // Write a chunk of text_data from offset_in_text to lob_page.
    //   //      // Update remaining_len, offset_in_text.
    //   //      // Mark lob_page as dirty. Unpin.
    //   //    }
    //   //    // Store first_overflow_page_id and actual_length in the main record slot for this TEXT field.
    //   // }
    //   // ... finally insert the main record part ...
    // }

    // When reading a record with TEXT data:
    // std::string read_text_field(Table* table, const RID& main_record_rid, int text_field_idx) {
    //   // 1. Read main record from main_record_rid.
    //   // 2. Deserialize to get the TextFieldPtr (or similar descriptor) for text_field_idx.
    //   //    TextFieldPtr* lob_desc = get_lob_descriptor_from_main_record(main_record_data, text_field_idx);
    //   //    std::string result_string;
    //   //    result_string.reserve(lob_desc->actual_length);
    //   //
    //   //    // Append in-row part if any
    //   //    if (lob_desc->in_row_length > 0) {
    //   //      result_string.append(lob_desc->in_row_data, lob_desc->in_row_length);
    //   //    }
    //   //
    //   //    PageNum current_overflow_page_id = lob_desc->first_overflow_page_id;
    //   //    while (current_overflow_page_id != NULL_PAGE_ID) {
    //   //      Page* lob_page = buffer_pool_manager_->fetch_page(table->get_lob_file_id(), current_overflow_page_id);
    //   //      // Read data chunk from lob_page.
    //   //      // Append to result_string.
    //   //      // current_overflow_page_id = RecordPageHandler::get_next_lob_page(lob_page_header_or_data);
    //   //      // buffer_pool_manager_->unpin_page(lob_page->get_page_id(), false);
    //   //    }
    //   //    return result_string;
    // }
};
\end{lstlisting}

\paragraph{页面格式与管理}
数据页的格式需要能够区分普通数据和指向辅助页的LOB描述符。辅助页本身也需要一个头部来存储元信息，例如该页存储的数据分片长度以及指向下一个辅助页的指针。缓冲池管理器 (\texttt{src/observer/storage/buffer/}) 在此过程中负责所有页面的读写和缓存，无需特殊改动，但其效率对LOB操作性能有直接影响。

\paragraph{事务与日志}
对跨页存储的 \texttt{TEXT} 字段的修改操作（插入、更新、删除部分或全部）必须是原子的。这意味着相关的日志记录 (\texttt{CLogRecord}) 需要能够描述对多个页面的修改。例如，更新一个大型 \texttt{TEXT} 字段可能涉及释放旧的辅助页链、分配新的辅助页链，并在主记录中更新描述符。这些操作序列需要整体记录到日志中，以便能够正确地进行回滚或恢复。日志管理器 (\texttt{src/observer/storage/clog/}) 可能需要更复杂的日志记录类型来支持此类操作。

\paragraph{查询与操作}
大部分对 \texttt{TEXT} 字段的操作（如比较、模式匹配 \texttt{LIKE}）在获取完整字段内容后，在表达式层面执行，与常规 \texttt{VARCHAR} 类似，但需注意性能影响，因为读取和重组大型 \texttt{TEXT} 字段可能非常耗时和消耗内存。部分操作，如获取长度 (\texttt{LENGTH()})，可以直接从LOB描述符中获取，无需读取所有辅助页。
索引 \texttt{TEXT} 字段通常不被允许或仅支持前缀索引，因为其内容过大。

通过上述对SQL解析、元数据、记录管理和日志系统的扩展，可以实现对 \texttt{TEXT} 类型字段的支持，并有效管理超过单页限制的超大数据。
