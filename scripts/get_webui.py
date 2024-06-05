import os
import requests
import tarfile

GITHUB_RELEASES_URL = "https://api.github.com/repos/KamranAghlami/RCLinkWebUI/releases/latest"
OUTPUT_DIRECTORY = os.path.join(os.path.dirname(__file__), '../main/app/web')

def download_file(url, output):
    with requests.get(url, stream=True) as response:
        response.raise_for_status()

        with open(output, 'wb') as file:
            for chunk in response.iter_content(chunk_size=8192):
                file.write(chunk)

def extract_file(file_path, output_directory):
    with tarfile.open(file_path, 'r') as tar:
        tar.extractall(path=output_directory)

if __name__ == "__main__":
    try:
        if not os.path.exists(OUTPUT_DIRECTORY):
            os.makedirs(OUTPUT_DIRECTORY)
            
            response = requests.get(GITHUB_RELEASES_URL)
            response.raise_for_status()
            assets = response.json()['assets']

            for asset in assets:
                file_path = os.path.join(OUTPUT_DIRECTORY, asset['name'])

                download_file(asset['browser_download_url'], file_path)

                if asset['name'].endswith('.tar.gz'):
                    extract_file(file_path, OUTPUT_DIRECTORY)

                    os.remove(file_path)

    except Exception as e:
        print("[main] error:", str(e))
