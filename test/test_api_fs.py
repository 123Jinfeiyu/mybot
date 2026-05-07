import urllib.request
import urllib.error
import json
import sys

def test_fs_api(api_url, mode, path):
    """
    测试文件系统API
    """
    # 构建请求数据
    data = {
        "mode": mode,
        "path": path
    }
    
    # 转换为JSON字符串并编码为字节
    json_data = json.dumps(data).encode('utf-8')
    
    # 创建请求对象
    request = urllib.request.Request(
        api_url,
        data=json_data,
        headers={
            'Content-Type': 'application/json',
            'Content-Length': len(json_data)
        },
        method='POST'
    )
    
    try:
        # 发送请求
        with urllib.request.urlopen(request, timeout=10) as response:
            # 读取响应
            response_data = response.read().decode('utf-8')
            
            print(f"✓ 请求成功!")
            print(f"状态码: {response.status}")
            print(f"响应内容: {response_data}")
            
            # 尝试解析JSON响应
            try:
                json_response = json.loads(response_data)
                print(f"JSON响应: {json.dumps(json_response, indent=2, ensure_ascii=False)}")
            except:
                pass
                
            return response_data
            
    except urllib.error.HTTPError as e:
        print(f"✗ HTTP错误: {e.code} - {e.reason}")
        try:
            error_body = e.read().decode('utf-8')
            print(f"错误详情: {error_body}")
        except:
            pass
        return None
        
    except urllib.error.URLError as e:
        print(f"✗ URL错误: {e.reason}")
        return None
        
    except Exception as e:
        print(f"✗ 其他错误: {type(e).__name__}: {e}")
        return None

def main():
    # 默认配置
    api_url = "http://127.0.0.1:5119/api/v1/fs"
    mode = "list"
    path = "C:/"
    
    # 解析命令行参数
    if len(sys.argv) > 1:
        path = sys.argv[1]
    if len(sys.argv) > 2:
        mode = sys.argv[2]
    if len(sys.argv) > 3:
        api_url = sys.argv[3]
    
    print(f"测试 API: {api_url}")
    print(f"模式: {mode}")
    print(f"路径: {path}")
    print("-" * 50)
    
    # 测试路径列表
    test_paths = [
        path,  # 用户指定的路径
        ".",   # 当前目录
        "./",  # 当前目录(带斜杠)
        "..",  # 上级目录
        "/",   # 根目录(如果服务器是Linux)
        "C:\\", # Windows格式
        "C:/Windows/Temp",  # 临时目录
        "D:/", # D盘
    ]
    
    print("开始测试不同路径格式...\n")
    
    for test_path in test_paths[:3]:  # 只测试前3个路径，避免太多
        print(f">>> 测试路径: '{test_path}'")
        result = test_fs_api(api_url, mode, test_path)
        print("-" * 50)
        
        if result:
            # 如果成功，就停止测试
            print("✓ 找到可用的路径格式!")
            break
    else:
        print("✗ 所有测试路径都失败了")
        print("\n建议检查:")
        print("1. 服务器是否正常运行: http://127.0.0.1:5119")
        print("2. 服务器程序是否有文件系统访问权限")
        print("3. 查看服务器控制台输出或日志文件")
        print("4. 确认API路径是否正确 (/api/v1/fs)")

if __name__ == "__main__":
    main()