#!/bin/sh
# Generates the C headers the firmware embeds:
#   main/packet_forwarder/webpage.h     <- webpage.html
#   main/packet_forwarder/global_json.h <- global_conf.json/*.json
# Both files are in .gitignore: run this script (or scripts/build.sh) after cloning.
set -e
cd "$(dirname "$0")/.."

pkt_dir=main/packet_forwarder
conf_dir=$pkt_dir/global_conf.json
hd_name=$pkt_dir/global_json.h

python3 scripts/dump_html.py $pkt_dir/webpage.html webpage_str > $pkt_dir/webpage.h

: > $hd_name
dump() {   # <json file> <C array name>
	printf '// dump from %s as string array\n' "$(basename "$1")" >> $hd_name
	printf 'const static uint8_t %s[] = {\n' "$2" >> $hd_name
	python3 scripts/json_to_hex_array.py "$1" >> $hd_name
	printf '};\n\n' >> $hd_name
}
dump $conf_dir/global_conf.cn490.json     global_cn_conf
dump $conf_dir/global_conf.eu868.json     global_eu_conf
dump $conf_dir/global_conf.us915.json     global_us_conf
dump $conf_dir/global_conf.au915_sb0.json global_au0_conf
dump $conf_dir/global_conf.au915_sb1.json global_au1_conf

sed -i 's/^0x/    0x/; s/ *$//' $hd_name
echo "headers generated: $pkt_dir/webpage.h $hd_name"
