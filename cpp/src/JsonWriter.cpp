#include "JsonWriter.hpp"

#include <ostream>
#include <sstream>
#include <string_view>

namespace {
constexpr char kHexDigits[] = "0123456789abcdef";

void writeEscaped(std::ostream& output, std::string_view value) {
    for (unsigned char character : value) {
        switch (character) {
            case '\\':
                output << "\\\\";
                break;
            case '"':
                output << "\\\"";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            default:
                if (character < 0x20) {
                    output << "\\u00"
                           << kHexDigits[(character >> 4) & 0x0f]
                           << kHexDigits[character & 0x0f];
                } else {
                    output.put(static_cast<char>(character));
                }
                break;
        }
    }
}

void writeQuotedEscaped(std::ostream& output, std::string_view value) {
    output.put('"');
    writeEscaped(output, value);
    output.put('"');
}
}

std::string JsonWriter::write(
    const std::filesystem::path& rootPath,
    const std::vector<analysis::FileAnalysis>& analysis
) {
    std::ostringstream json;
    write(json, rootPath, analysis);
    return json.str();
}

void JsonWriter::write(
    std::ostream& output,
    const std::filesystem::path& rootPath,
    const std::vector<analysis::FileAnalysis>& analysis
) {
    output << "{\n  \"root\": ";
    writeQuotedEscaped(output, rootPath.generic_string());
    output << ",\n  \"files\": [\n";

    bool firstFile = true;
    for (const auto& file : analysis) {
        if (!firstFile) {
            output << ",\n";
        }
        firstFile = false;

        output << "    {\n";
        output << "      \"path\": ";
        writeQuotedEscaped(output, file.filePath.generic_string());
        output << ",\n";
        output << "      \"headers\": [";

        bool firstHeader = true;
        for (const auto& header : file.headers) {
            if (!firstHeader) {
                output << ", ";
            }
            firstHeader = false;
            writeQuotedEscaped(output, header);
        }

        output << "],\n";
        output << "      \"classes\": [\n";

        bool firstClass = true;
        for (const auto& classInfo : file.classes) {
            if (!firstClass) {
                output << ",\n";
            }
            firstClass = false;

            output << "        {\n";
            output << "          \"name\": ";
            writeQuotedEscaped(output, classInfo.name);
            output << ",\n";
            output << "          \"public_methods\": [";

            bool firstMethod = true;
            for (const auto& method : classInfo.publicMethods) {
                if (!firstMethod) {
                    output << ", ";
                }
                firstMethod = false;
                writeQuotedEscaped(output, method);
            }

            output << "]\n";
            output << "        }";
        }

        output << "\n      ]\n";
        output << "    }";
    }

    output << "\n  ]\n}\n";
}
