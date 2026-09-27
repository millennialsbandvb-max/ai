# Add or remove NAM MPC in MPC.settings' plugin list (BusyBox awk compatible).
#   awk -v mode=add -v file=/sdcard/vst/NAM-MPC.so -v entryfile=plugin.xml -f plugin_list.awk MPC.settings
#   awk -v mode=remove -v file=/sdcard/vst/NAM-MPC.so -f plugin_list.awk MPC.settings
# Every <PLUGIN .../> element whose file= is ours is dropped (even one split over several lines); in add mode the
# entry goes into <VALUE name="pluginList-arm"><KNOWNPLUGINS>, which is created before </PROPERTIES> if missing.
BEGIN {
    if (mode == "add") { while ((getline l < entryfile) > 0) entry = entry l; close(entryfile) }
    ours = "file=\"" file "\""; inlist = 0; added = 0; held = ""
}
function indent(s) { sub(/<.*/, "", s); return s }
held != "" {                      # inside a multi-line <PLUGIN ...
    held = held "\n" $0
    if ($0 ~ /\/>/) { if (index(held, ours) == 0) print held; held = "" }
    next
}
/<PLUGIN[ \t]/ || /<PLUGIN$/ {
    if ($0 ~ /\/>/) { if (index($0, ours) == 0) print; next }
    held = $0; next
}
/<VALUE name="pluginList-arm">/ { inlist = 1; print; next }
inlist && /<KNOWNPLUGINS\/>/ {
    if (mode == "add") { i = indent($0); print i "<KNOWNPLUGINS>"; print i "  " entry; print i "</KNOWNPLUGINS>"; added = 1 }
    else print
    inlist = 0; next
}
inlist && /<\/KNOWNPLUGINS>/ {
    if (mode == "add" && !added) { print indent($0) "  " entry; added = 1 }
    inlist = 0; print; next
}
/<\/PROPERTIES>/ && mode == "add" && !added {
    print "  <VALUE name=\"pluginList-arm\">"; print "    <KNOWNPLUGINS>"; print "      " entry
    print "    </KNOWNPLUGINS>"; print "  </VALUE>"; added = 1
}
{ print }
