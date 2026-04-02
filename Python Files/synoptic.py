import requests
import time
from datetime import datetime

# API endpoint and configuration
API_URL = "https://api.synopticdata.com/v2/stations/timeseries?STID=KMDW&showemptystations=1&units=temp|F,speed|mph,english&recent=4320&complete=1&token=7c76618b66c74aee913bdbae4b448bdd&obtimezone=local"
HEADERS = {
    "origin": "https://www.weather.gov"
}
TARGET_TIMESTAMP = "2026-01-14T19:10:00-0600"
POLL_INTERVAL = 1  # seconds

def get_latest_date_time(response_data):
    """
    Extract the latest date_time from the OBSERVATIONS section.
    Returns None if not found or if observations are empty.
    """
    try:
        if "STATION" in response_data and len(response_data["STATION"]) > 0:
            station = response_data["STATION"][0]
            if "OBSERVATIONS" in station and "date_time" in station["OBSERVATIONS"]:
                date_times = station["OBSERVATIONS"]["date_time"]
                if date_times and len(date_times) > 0:
                    # Return the last (latest) date_time
                    return date_times[-1]
    except (KeyError, IndexError, TypeError) as e:
        print(f"Error parsing response: {e}")
    return None

def main():
    print(f"Starting to poll API every {POLL_INTERVAL} second(s)...")
    print(f"Target timestamp: {TARGET_TIMESTAMP}")
    print("Waiting for the latest observation to match target timestamp...\n")
    
    while True:
        try:
            # Make API request with custom header
            response = requests.get(API_URL, headers=HEADERS, timeout=10)
            response.raise_for_status()
            
            # Parse JSON response
            data = response.json()
            
            # Get the latest date_time from observations
            latest_date_time = get_latest_date_time(data)
            
            if latest_date_time:
                current_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")
                print(f"[{current_time}] Latest observation: {latest_date_time}")
                
                # Check if the latest date_time matches the target
                if latest_date_time == TARGET_TIMESTAMP:
                    match_timestamp = datetime.now()
                    print("\n" + "="*60)
                    print("MATCH FOUND!")
                    print(f"Target timestamp matched: {TARGET_TIMESTAMP}")
                    print(f"Recorded timestamp: {match_timestamp.strftime('%Y-%m-%d %H:%M:%S.%f')}")
                    print(f"Recorded timestamp (ISO): {match_timestamp.isoformat()}")
                    print("="*60)
                    break
            else:
                print(f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')}] No observations found in response")
            
        except requests.exceptions.RequestException as e:
            print(f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')}] API request error: {e}")
        except Exception as e:
            print(f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')}] Unexpected error: {e}")
        
        # Wait before next poll
        time.sleep(POLL_INTERVAL)

if __name__ == "__main__":
    main()
