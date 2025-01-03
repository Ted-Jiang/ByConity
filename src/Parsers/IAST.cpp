#include <Parsers/IAST.h>

#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <IO/Operators.h>
#include <Common/SipHash.h>
#include <Common/SensitiveDataMasker.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int TOO_BIG_AST;
    extern const int TOO_DEEP_AST;
    extern const int BAD_ARGUMENTS;
    extern const int UNKNOWN_ELEMENT_IN_AST;
}


const char * IAST::hilite_keyword      = "\033[1m";
const char * IAST::hilite_identifier   = "\033[0;36m";
const char * IAST::hilite_function     = "\033[0;33m";
const char * IAST::hilite_operator     = "\033[1;33m";
const char * IAST::hilite_alias        = "\033[0;32m";
const char * IAST::hilite_substitution = "\033[1;36m";
const char * IAST::hilite_none         = "\033[0m";


size_t IAST::size() const
{
    size_t res = 1;
    for (const auto & child : children)
        res += child->size();

    return res;
}

size_t IAST::checkSize(size_t max_size) const
{
    size_t res = 1;
    for (const auto & child : children)
        res += child->checkSize(max_size);

    if (res > max_size)
        throw Exception("AST is too big. Maximum: " + toString(max_size), ErrorCodes::TOO_BIG_AST);

    return res;
}


IAST::Hash IAST::getTreeHash() const
{
    SipHash hash_state;
    updateTreeHash(hash_state);
    IAST::Hash res;
    hash_state.get128(res);
    return res;
}


void IAST::updateTreeHash(SipHash & hash_state) const
{
    updateTreeHashImpl(hash_state);
    hash_state.update(children.size());
    for (const auto & child : children)
        child->updateTreeHash(hash_state);
}


void IAST::updateTreeHashImpl(SipHash & hash_state) const
{
    auto id = getID();
    hash_state.update(id.data(), id.size());
}


size_t IAST::checkDepthImpl(size_t max_depth, size_t level) const
{
    size_t res = level + 1;
    for (const auto & child : children)
    {
        if (level >= max_depth)
            throw Exception("AST is too deep. Maximum: " + toString(max_depth), ErrorCodes::TOO_DEEP_AST);
        res = std::max(res, child->checkDepthImpl(max_depth, level + 1));
    }

    return res;
}

String IAST::formatWithHiddenSecrets(size_t max_length, bool one_line, bool no_alias, DialectType dialect, bool remove_tenant_id) const
{
    WriteBufferFromOwnString buf;
    FormatSettings settings{buf, one_line, no_alias};
    settings.show_secrets = false;
    settings.dialect_type = dialect;
    settings.remove_tenant_id = remove_tenant_id;
    format(settings);

    return wipeSensitiveDataAndCutToLength(buf.str(), max_length);
}

bool IAST::childrenHaveSecretParts() const
{
    for (const auto & child : children)
    {
        if (child->hasSecretParts())
            return true;
    }
    return false;
}

void IAST::cloneChildren()
{
    for (auto & child : children)
        child = child->clone();
}


String IAST::getColumnName() const
{
    WriteBufferFromOwnString write_buffer;
    appendColumnName(write_buffer);
    return write_buffer.str();
}


String IAST::getColumnNameWithoutAlias() const
{
    WriteBufferFromOwnString write_buffer;
    appendColumnNameWithoutAlias(write_buffer);
    return write_buffer.str();
}


void IAST::FormatSettings::writeIdentifier(const String & name) const
{
    switch (identifier_quoting_style)
    {
        case IdentifierQuotingStyle::None:
        {
            if (always_quote_identifiers)
                throw Exception("Incompatible arguments: always_quote_identifiers = true && identifier_quoting_style == IdentifierQuotingStyle::None",
                    ErrorCodes::BAD_ARGUMENTS);
            writeString(name, ostr);
            break;
        }
        case IdentifierQuotingStyle::Backticks:
        {
            if (always_quote_identifiers)
                writeBackQuotedString(name, ostr);
            else
                writeProbablyBackQuotedString(name, ostr);
            break;
        }
        case IdentifierQuotingStyle::DoubleQuotes:
        {
            if (always_quote_identifiers)
                writeDoubleQuotedString(name, ostr);
            else
                writeProbablyDoubleQuotedString(name, ostr);
            break;
        }
        case IdentifierQuotingStyle::BackticksMySQL:
        {
            if (always_quote_identifiers)
                writeBackQuotedStringMySQL(name, ostr);
            else
                writeProbablyBackQuotedStringMySQL(name, ostr);
            break;
        }
    }
}

void IAST::dumpTree(WriteBuffer & ostr, size_t indent) const
{
    String indent_str(indent, '-');
    ostr << indent_str << getID() << ", ";
    writePointerHex(this, ostr);
    writeChar('\n', ostr);
    for (const auto & child : children)
    {
        if (!child) throw Exception("Can't dump nullptr child", ErrorCodes::UNKNOWN_ELEMENT_IN_AST);
        child->dumpTree(ostr, indent + 1);
    }
}

std::string IAST::dumpTree(size_t indent) const
{
    WriteBufferFromOwnString wb;
    dumpTree(wb, indent);
    return wb.str();
}

void SqlHint::serialize(WriteBuffer & buf) const
{
    writeBinary(name, buf);
    writeBinary(options.size(), buf);
    for (const auto & option : options)
        writeBinary(option, buf);

    writeBinary(kv_options.size(), buf);
    for (const auto & item : kv_options)
    {
        writeBinary(item.first, buf);
        writeBinary(item.second, buf);
    }
}

SqlHint SqlHint::deserialize(ReadBuffer & buf)
{
    String name;
    readBinary(name, buf);
    SqlHint hint{name};

    size_t size;
    readBinary(size, buf);

    for (size_t i = 0; i < size; ++i)
    {
        String option;
        readBinary(option, buf);
        hint.setOption(option);
    }

    readBinary(size, buf);
    for (size_t i = 0; i < size; ++i)
    {
        String key;
        readBinary(key, buf);
        String value;
        readBinary(value, buf);
        hint.setKvOption(key, value);
    }

    return hint;
}

void SqlHints::serialize(WriteBuffer & buf) const
{
    writeBinary(size(), buf);
    for (const auto & hint : *this)
        hint.serialize(buf);
}

void SqlHints::deserialize(ReadBuffer & buf)
{
    size_t size;
    readBinary(size, buf);
    for (size_t i = 0; i < size; ++i)
    {
        SqlHint hint = SqlHint::deserialize(buf);
        this->push_back(hint);
    }
}

}
