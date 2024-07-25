import xml.etree.ElementTree as ET
import subprocess
import os
import sys

doxygen_path = sys.argv[1]

if not os.path.exists(doxygen_path):
	print(doxygen_path + " is not a directory")
	print ("Script usage: \"./generate_module_docs.py {path-to-doxygen-xml-output}\"")
	sys.exit()

file_class_dict = {}
for file in os.listdir(doxygen_path):
	if ("class" in file or "struct" in file) and ".xml" in file:
		xml_tree = ET.parse(doxygen_path + "/" + file)
		root = xml_tree.getroot()
		# class_name = file.rstrip(".xml").lstrip("class")
		object_name = root[0][0].text
		for child in root[0]:
			if child.tag == "location":
				file_name = os.path.basename(child.attrib['file'])
				if file_name not in file_class_dict.keys():
					file_class_dict[file_name] = []
				file_class_dict[file_name].append([object_name, root[0].attrib['kind']])
				
rst_file = open("modules.rst", "w")
rst_file.write("Modules\n")
rst_file.write("=======\n")
rst_file.write("\n")

for key in file_class_dict.keys():
	rst_file.write(key + "\n")
	for x in range(len(key)):
		rst_file.write("-")
	rst_file.write("\n\n")
	for item_name, item_type in file_class_dict[key]:
		if "class" in item_type:
			rst_file.write(".. doxygenclass:: " + item_name + "\n")
		else:
			rst_file.write(".. doxygenstruct:: " + item_name + "\n")
		rst_file.write("   :project: ivi-homescreen\n")
		rst_file.write("   :members:\n")
		rst_file.write("   :private-members:\n")
		rst_file.write("   :undoc-members:\n\n")



