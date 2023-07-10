import os

def remove_c_files(directory):
    for root, dirs, files in os.walk(directory):
        for file in files:
            if file.endswith(".c"):
               file_path = os.path.join(root, file)
               os.remove(file_path)
               print(f"Deleted {file_path}")

# 替换为你的SDK目录路径
sdk_directory = "./"

remove_c_files(sdk_directory)

