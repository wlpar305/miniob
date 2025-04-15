\subsection{日期类型（DATE）的实现}

在MiniOB系统中，实现日期类型（DATE）的字段需要修改从词法解析、语法解析、到存储执行等多个组件。本节详细分析如何实现支持日期类型的完整流程。

\paragraph{日期的存储结构设计}
日期类型在系统中采用整型（int）存储，以YYYYMMDD格式保存，如"2021-10-21"存储为整数20211021。这种设计既简化了日期比较操作，又便于日期的格式化显示。对于存储空间，使用4字节整数与系统中已有的INTS类型保持一致。

\paragraph{词法分析与语法分析的修改}
首先，在词法分析器中添加对DATE类型的支持。在`lex_sql.l`文件中增加日期类型的TOKEN识别：

```c
DATE                                    RETURN_TOKEN(DATE_T);
{QUOTE}([0-9]{4})\-([0-9]|[0-9]{2})\-([0-9]|[0-9]{2}){QUOTE} yylval->string=strdup(yytext); RETURN_TOKEN(DATE_STR);
```

规则使用正则表达式匹配形如'YYYY-MM-DD'或'YYYY-M-D'的日期格式，M和D可以是一位或两位数字。

在语法分析器`yacc_sql.y`中添加对DATE_T类型的识别和处理：

```c
type:
    INT_T      { $$=INTS; }
    | STRING_T { $$=CHARS; }
    | FLOAT_T  { $$=FLOATS; }
    | DATE_T   { $$=DATES;}
    | TEXT_T   { $$=TEXTS; }
    ;
```

同时修改值处理部分，增加对DATE_STR的处理逻辑：

```c
value:
    ...
    |DATE_STR {
      char *tmp = common::substr($1,1,strlen($1)-2);
      std::string str(tmp);
      Value * value = new Value();
      int date;
      if(string_to_date(str,date) < 0) {
        yyerror(&@$,sql_string,sql_result,scanner,"date invaid",true);
        YYERROR;
      }
      else
      {
        value->set_date(date);
      }
      $$ = value;
      free(tmp);
    }
    ...
```

\paragraph{日期类型的定义与转换}
在系统中定义了枚举类型`AttrType`增加DATES类型：

```c
enum AttrType
{
  UNDEFINED,
  CHARS,          ///< 字符串类型
  INTS,           ///< 整数类型(4字节)
  FLOATS,         ///< 浮点数类型(4字节)
  DOUBLES,        
  DATES,          ///< 日期类型
  LONGS,          ///< Int64
  TEXTS,          ///< text类型，最大65535字节
  NULLS,          ///< null类型
  BOOLEANS,       ///< boolean类型，当前不是由parser解析出来的，是程序内部使用的
};
```

系统中实现了日期字符串和整数之间的转换函数：

```c
int string_to_date(const std::string &str,int32_t & date)
{
    int y,m,d;
    sscanf(str.c_str(), "%d-%d-%d", &y, &m, &d);
    bool b = check_dateV2(y,m,d);
    if(!b) return -1;
    date = y*10000+m*100+d;
    return 0;
}

std::string date_to_string(int32_t date)
{
    std::string ans = "YYYY-MM-DD";
    std::string tmp = std::to_string(date);
    int tmp_index = 7;
    for(int i = 9 ; i >=0 ;i--)
    {
        if(i == 7|| i == 4)
        {
            ans[i] = '-';
        }
        else
        {
            ans[i] = tmp[tmp_index--];
        }
    }
    return ans;
}
```

\paragraph{日期有效性校验}
实现了`check_dateV2`函数检验日期是否合法，考虑闰年和各月份的不同天数：

```c
bool check_dateV2(int year, int month, int day)
{
  static int mon[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (year % 400 == 0 || (year % 100 && year % 4 == 0));
  if (year > 0 && (month > 0) && (month <= 12) && (day > 0) && (day <= ((month == 2 && leap) ? 1 : 0) + mon[month]))
    return true;
  else
    return false;
}
```

这个函数实现了：
1. 闰年的判断（能被400整除，或能被4整除但不能被100整除）
2. 月份范围检查（1-12）
3. 天数合法性检查（考虑平年/闰年二月和大小月）

\paragraph{Value类的扩展}
在`Value`类中添加对日期类型的支持，包括设置日期值、获取日期值和日期比较：

```c
void Value::set_date(int val)
{
  attr_type_ = DATES;
  num_value_.int_value_ = val;
  length_ = sizeof(val);
}
```

日期值的输出格式化处理：

```c
std::string Value::to_string() const
{
  ...
  case DATES:{
    os << date_to_string(num_value_.int_value_);
  } break;
  ...
}
```

日期值的比较操作，由于日期以整数形式存储，直接使用整数比较：

```c
int Value::compare(const Value &other) const
{
  ...
  case DATES: {
    return common::compare_int((void *)&this->num_value_.int_value_, (void *)&other.num_value_.int_value_);
  } break;
  ...
}
```

\paragraph{索引支持}
为了支持DATE类型作为索引，需要在B+树索引实现中添加对DATE类型的比较处理：

```c
switch (attr_type_[i]) {
  case INTS:
  case DATES: {
    if (0 == (cmp_res = common::compare_int((void *)(v1 + offset), (void *)(v2 + offset)))) {
      offset += attr_length_[i];
    } else {
      return cmp_res;
    }
    break;
  } 
  ...
}
```

在B+树输出函数中也需处理DATE类型：

```c
switch (attr_type_[idx]) {
  case INTS:
  case DATES: {
    key_str += std::to_string(*(int *)(v + offset));
    key_str += ",";
    offset += attr_length_[idx];
    break;
  }
  ...
}
```

\paragraph{数据记录加载}
在从文件加载数据时，也对DATE类型进行处理：

```c
switch (field->type()) {
  case INTS:
  case DATES: {
    deserialize_stream.clear();
    deserialize_stream.str(file_value);

    int int_value;
    deserialize_stream >> int_value;
    if (!deserialize_stream || !deserialize_stream.eof()) {
      errmsg << "need an integer but got '" << file_values[i] << "' (field index:" << i << ")";
      rc = RC::SCHEMA_FIELD_TYPE_MISMATCH;
    } else {
      record_values[i].set_int(int_value);
    }
  }
  break;
  ...
}
```

\paragraph{网络通信协议适配}
通过MySQL协议将结果返回给客户端时，需要将DATE类型映射为MySQL协议中的日期类型：

```c
enum enum_field_types 
{
  ...
  MYSQL_TYPE_DATE,
  ...
};
```

\paragraph{总结}
日期类型的实现涉及系统各个层面的修改，从词法解析、语法解析，到值存储、比较、索引支持和客户端展示。通过将日期以YYYYMMDD格式的整数存储，简化了日期比较操作，并能够方便地进行日期格式化展示。在日期有效性校验方面，实现了对闰年和大小月的处理，确保存入数据库的日期是合法有效的。这种实现方式既简单高效，又能满足常见的日期操作需求。
