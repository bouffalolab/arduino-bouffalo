import os
import hashlib
import zipfile
import tarfile
import json

def calculate_sha256(file_path):
    # 创建一个SHA-256哈希对象
    sha256_hash = hashlib.sha256()

    # 以二进制读取文件内容并更新哈希对象
    with open(file_path, "rb") as file:
        for chunk in iter(lambda: file.read(4096), b""):
            sha256_hash.update(chunk)

    # 获取SHA-256哈希值
    sha256_value = sha256_hash.hexdigest()

    return sha256_value

def calculate_file_size(file_path):
    # 获取文件大小（以字节为单位）
    file_size = os.path.getsize(file_path)

    return file_size

def create_tarbz2(source_paths, output_path, version):
    # 创建压缩文件名
    tarbz2_filename = f"{output_path}_{version}.tar.bz2"

    # 创建一个新的.tar.bz2文件
    with tarfile.open(tarbz2_filename, 'w:bz2') as tar:
        for source_path in source_paths:
            if os.path.isfile(source_path):
                # 如果是文件，则获取文件名并添加到压缩文件中
                file_name = os.path.basename(source_path)
                tar.add(source_path, arcname=file_name)
            elif os.path.isdir(source_path):
                # 如果是文件夹，则遍历文件夹并添加其中的文件到压缩文件中
                for root, dirs, files in os.walk(source_path):
                    for file in files:
                        file_path = os.path.join(root, file)
                        relative_path = os.path.relpath(file_path, source_path)
                        tar.add(file_path, arcname=os.path.join(os.path.basename(source_path), relative_path))

    print(f"生成压缩文件 {tarbz2_filename} 成功！")

def create_zip(source_paths, output_path, version):
    # 创建压缩文件名
    zip_filename = f"{output_path}_{version}.zip"

    # 创建一个新的zip文件
    with zipfile.ZipFile(zip_filename, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for source_path in source_paths:
            if os.path.isfile(source_path):
                # 如果是文件，则获取文件名并添加到压缩文件中
                file_name = os.path.basename(source_path)
                zipf.write(source_path, arcname=file_name)
            elif os.path.isdir(source_path):
                # 如果是文件夹，则遍历文件夹并添加其中的文件到压缩文件中
                for root, dirs, files in os.walk(source_path):
                    for file in files:
                        file_path = os.path.join(root, file)
                        relative_path = os.path.relpath(file_path, source_path)
                        zipf.write(file_path, arcname=os.path.join(os.path.basename(source_path), relative_path))

    print(f"生成压缩文件 {zip_filename} 成功！")

def create_cores_zip(source_paths, output_path, version):
    # 创建压缩文件名
    zip_filename = f"{output_path}_{version}.zip"

    # 创建一个新的zip文件
    with zipfile.ZipFile(zip_filename, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for source_path in source_paths:
            if os.path.isfile(source_path):
                # 如果是文件，则获取文件名并添加到压缩文件中
                file_name = os.path.basename(source_path)
                zipf.write(source_path, arcname=os.path.join(output_path, file_name))
            elif os.path.isdir(source_path):
                # 如果是文件夹，则遍历文件夹并添加其中的文件到压缩文件中
                for root, dirs, files in os.walk(source_path):
                    for file in files:
                        file_path = os.path.join(root, file)
                        relative_path = os.path.relpath(file_path, source_path)
                        arcname = os.path.join(output_path, os.path.basename(source_path), relative_path)
                        zipf.write(file_path, arcname=arcname)

    print(f"生成压缩文件 {zip_filename} 成功！")

# generate index json
def generate_index_json(package_name, maintainer, website_url, email, platforms, tools):
    data = {
      "packages": [
        {
          "name": package_name,
          "maintainer": maintainer,
          "websiteURL": website_url,
          "email": email,
          "help": {
            "online": website_url
          },
          "platforms": platforms,
          "tools": tools
        }
      ]
    }

    return data

def generate_platform(name, architecture, version, category, url, archive_filename, checksum, size, website_url, boards, tools_dependencies):
    platform = {
      "name": name,
      "architecture": architecture,
      "version": version,
      "category": category,
      "url": url,
      "archiveFileName": archive_filename,
      "checksum": checksum,
      "size": size,
      "help": {
        "online": website_url
      },
      "boards": boards,
      "toolsDependencies": tools_dependencies
    }

    return platform

def generate_tool(name, version, systems):
    tool = {
      "name": name,
      "version": version,
      "systems": systems
    }

    return tool

def generate_board(name):
    board = {
        "name": name
    }

    return board

def generate_tool_dependency(packager, version, name):
    tool_dependency = {
      "packager": packager,
      "version": version,
      "name": name
    }

    return tool_dependency

def generate_system(host, url, archive_filename, checksum, size):
    system = {
      "host": host,
      "url": url,
      "archiveFileName": archive_filename,
      "checksum": checksum,
      "size": size
    }

    return system


# bouffalolab
bouffalolab_paths = ["boards.txt","platform.txt","./cores", "./variants"]  # 源文件或文件夹的路径列表
bouffalolab_path = "bouffalolab"  # 输出文件的路径和名称前缀
cores_version = "1.0.1"  # 版本号

create_cores_zip(bouffalolab_paths, bouffalolab_path, cores_version)
bouffalolab_outname = f"{bouffalolab_path}_{cores_version}.zip"
bouffalolab_sha256 = calculate_sha256(bouffalolab_outname)
bouffalolab_size = calculate_file_size(bouffalolab_outname)
print(f"bouffalolab sha:{bouffalolab_sha256}, size:{bouffalolab_size}")

# tools
# xuantie
xuantie_paths = ["./tools/Xuantie-900-gcc"]
xuantie_path = "Xuantie-900-gcc"
compiler_version = "2.6.1"
create_zip(xuantie_paths, xuantie_path, compiler_version)
xuantie_outname = f"{xuantie_path}_{compiler_version}.zip"
xuantie_sha256 = calculate_sha256(xuantie_outname)
xuantie_size = calculate_file_size(xuantie_outname)
print(f"xuantie sha:{xuantie_sha256}, size:{xuantie_size}")

# flash tools
flash_tools_paths = ["./tools/bflb_flash_tools"]
flash_tools_path = "bflb_flash_tools"
flash_tools_version = "1.0.7"
create_zip(flash_tools_paths, flash_tools_path, flash_tools_version)
flash_tools_outname = f"{flash_tools_path}_{flash_tools_version}.zip"
flash_tools_sha256 = calculate_sha256(flash_tools_outname)
flash_tools_size = calculate_file_size(flash_tools_outname)
print(f"flash tools sha:{flash_tools_sha256}, size:{flash_tools_size}")

# fw post tools
fw_post_paths = ["./tools/bflb_fw_post_proc"]
fw_post_path = "bflb_fw_post_proc"
fw_version = "1.0.0"
create_zip(fw_post_paths, fw_post_path, fw_version)
fw_post_outname = f"{fw_post_path}_{fw_version}.zip"
fw_post_sha256 = calculate_sha256(fw_post_outname)
fw_post_size = calculate_file_size(fw_post_outname)
print(f"fw_post sha:{fw_post_sha256}, size:{fw_post_size}")


# 生成 JSON 数据

package_name = "bouffalolab"
maintainer = "BH6BAO"
website_url = "https://github.com/strongwong/arduino-bl618"
email = "qqwang@bouffalolab.com"
index_version = "1.0.1"

platforms = [
    generate_platform(
        name=package_name,
        architecture="bouffalolab",
        version=f"{cores_version}",
        category="BouffaloLab",
        url=f"https://github.com/strongwong/arduino-bl618/releases/download/{index_version}/{bouffalolab_outname}",
        archive_filename=bouffalolab_outname,
        checksum=f"SHA-256:{bouffalolab_sha256}",
        size=f"{bouffalolab_size}",
        website_url=website_url,
        boards=[
            generate_board(name="BL618G0")
        ],
        tools_dependencies=[
            generate_tool_dependency(packager=package_name, version=f"{fw_version}", name=fw_post_path),
            generate_tool_dependency(packager=package_name, version=f"{flash_tools_version}", name=flash_tools_path),
            generate_tool_dependency(packager=package_name, version=f"{compiler_version}", name=xuantie_path)
        ]
    )
]

tools = [
    generate_tool(
        name=fw_post_path,
        version=f"{fw_version}",
        systems=[
            generate_system(
                host="i686-mingw32",
                url=f"https://github.com/strongwong/arduino-bl618/releases/download/{index_version}/{fw_post_outname}",
                archive_filename=fw_post_outname,
                checksum=f"SHA-256:{fw_post_sha256}",
                size=f"{fw_post_size}"
            )
        ]
    ),
    generate_tool(
        name=flash_tools_path,
        version=f"{flash_tools_version}",
        systems=[
            generate_system(
                host="i686-mingw32",
                url=f"https://github.com/strongwong/arduino-bl618/releases/download/{index_version}/{flash_tools_outname}",
                archive_filename=flash_tools_outname,
                checksum=f"SHA-256:{flash_tools_sha256}",
                size=f"{flash_tools_size}"
            )
        ]
    ),
    generate_tool(
        name=xuantie_path,
        version=f"{compiler_version}",
        systems=[
            generate_system(
                host="i686-mingw32",
                url=f"https://github.com/strongwong/arduino-bl618/releases/download/{index_version}/{xuantie_outname}",
                archive_filename=xuantie_outname,
                checksum=f"SHA-256:{xuantie_sha256}",
                size=f"{xuantie_size}"
            )
        ]
    )
]

data = generate_index_json(package_name, maintainer, website_url, email, platforms, tools)
# 将数据写入文件
filename = "package_bouffalolab_index.json"
with open(filename, "w") as f:
    json.dump(data, f, indent=4)