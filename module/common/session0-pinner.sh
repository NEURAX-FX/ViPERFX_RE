#!/system/bin/sh

pin_viper_xml_music_effect() {
    xml_target_file="$1"
    if grep -q '<apply effect="v4a_standard_re"/>' "$xml_target_file"; then return 0; fi
    xml_tmp_file="${xml_target_file}.viper-session0.$$"
    awk '
        function indent(line) { match(line, /^[[:space:]]*/); return substr(line, 1, RLENGTH) }
        BEGIN { in_post=0; in_music=0; post_seen=0; inserted=0 }
        {
            line=$0
            if (line ~ /<postprocess([[:space:]>])/) { in_post=1; post_seen=1; post_indent=indent(line) }
            if (in_post && line ~ /<stream[^>]*type="music"/) { in_music=1; stream_indent=indent(line) }
            if (in_music && line ~ /<\/stream>/ && !inserted) {
                print stream_indent "    <!-- ViPER session0 pin -->"
                print stream_indent "    <apply effect=\"v4a_standard_re\"/>"
                inserted=1
            }
            if (in_post && line ~ /<\/postprocess>/ && !inserted) {
                print post_indent "    <stream type=\"music\">"
                print post_indent "        <!-- ViPER session0 pin -->"
                print post_indent "        <apply effect=\"v4a_standard_re\"/>"
                print post_indent "    </stream>"
                inserted=1
            }
            if (!post_seen && line ~ /<\/audio_effects_conf>/ && !inserted) {
                root_indent=indent(line)
                print root_indent "    <postprocess>"
                print root_indent "        <stream type=\"music\">"
                print root_indent "            <!-- ViPER session0 pin -->"
                print root_indent "            <apply effect=\"v4a_standard_re\"/>"
                print root_indent "        </stream>"
                print root_indent "    </postprocess>"
                inserted=1
            }
            print line
            if (line ~ /<\/stream>/) in_music=0
            if (line ~ /<\/postprocess>/) in_post=0
        }
        END { if (!inserted) exit 1 }
    ' "$xml_target_file" > "$xml_tmp_file" || { rm -f "$xml_tmp_file"; return 1; }
    if validate_viper_session0_effect "$xml_tmp_file"; then
        mv -f "$xml_tmp_file" "$xml_target_file"
        return 0
    fi
    rm -f "$xml_tmp_file"
    return 1
}

pin_viper_legacy_music_effect() {
    conf_target_file="$1"
    if grep -q '# ViPER session0 pin' "$conf_target_file"; then return 0; fi
    conf_tmp_file="${conf_target_file}.viper-session0.$$"
    awk '
        function count_char(text, char, total, rest) {
            total=0; rest=text
            while (index(rest, char) != 0) { total++; rest=substr(rest, index(rest,char)+1) }
            return total
        }
        function indent(line) { match(line, /^[[:space:]]*/); return substr(line, 1, RLENGTH) }
        BEGIN { depth=0; output_seen=0; music_seen=0; in_output=0; in_music=0; inserted=0 }
        {
            line=$0; trimmed=line; sub(/^[[:space:]]*/, "", trimmed)
            opens=count_char(line,"{"); closes=count_char(line,"}")
            if (!in_output && trimmed ~ /^output_session_processing[[:space:]]*{/) {
                in_output=1; output_seen=1; output_depth=depth+opens; output_indent=indent(line)
            } else if (in_output && !in_music && trimmed ~ /^music[[:space:]]*{/) {
                in_music=1; music_seen=1; music_depth=depth+opens; music_indent=indent(line)
            }
            if (in_music && trimmed ~ /^}/ && depth==music_depth && !inserted) {
                print music_indent "  # ViPER session0 pin"
                print music_indent "  v4a_standard_re {"
                print music_indent "  }"
                inserted=1
            }
            if (in_output && !music_seen && trimmed ~ /^}/ && depth==output_depth && !inserted) {
                print output_indent "  music {"
                print output_indent "    # ViPER session0 pin"
                print output_indent "    v4a_standard_re {"
                print output_indent "    }"
                print output_indent "  }"
                inserted=1
            }
            print line
            depth += opens-closes
            if (in_music && depth<music_depth) in_music=0
            if (in_output && depth<output_depth) in_output=0
        }
        END {
            if (!output_seen) {
                print "output_session_processing {"
                print "  music {"
                print "    # ViPER session0 pin"
                print "    v4a_standard_re {"
                print "    }"
                print "  }"
                print "}"
                inserted=1
            }
            if (!inserted) exit 1
        }
    ' "$conf_target_file" > "$conf_tmp_file" || { rm -f "$conf_tmp_file"; return 1; }
    if validate_viper_session0_effect "$conf_tmp_file"; then
        mv -f "$conf_tmp_file" "$conf_target_file"
        return 0
    fi
    rm -f "$conf_tmp_file"
    return 1
}

pin_viper_session0_effect() {
    case "$1" in
        *.xml) pin_viper_xml_music_effect "$1" ;;
        *.conf) pin_viper_legacy_music_effect "$1" ;;
        *) return 1 ;;
    esac
}

validate_viper_session0_effect() {
    validation_file="$1"
    if grep -q '<audio_effects_conf' "$validation_file"; then
            [ "$(grep -c '<apply effect="v4a_standard_re"/>' "$1")" -eq 1 ]
            grep -q '<stream type="music">' "$1"
    elif grep -q 'output_session_processing' "$validation_file"; then
            [ "$(grep -c '# ViPER session0 pin' "$1")" -eq 1 ]
            grep -q 'output_session_processing' "$1"
            grep -q '^[[:space:]]*music[[:space:]]*{' "$1"
    else
        return 1
    fi
}
